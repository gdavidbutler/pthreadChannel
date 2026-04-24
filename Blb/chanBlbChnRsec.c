/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2025 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of pthreadChannel
 *
 * pthreadChannel is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pthreadChannel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <time.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "rsec.h"
#include "chanBlbChnRsec.h"

unsigned int
chanBlbChnRsecShard(
  const struct chanBlbChnRsecCtx *ctx
){
  unsigned int overhead;

  overhead = ctx->tagSize + ctx->hmacSize + 8;
  if (ctx->dgramMax <= overhead
   || ctx->dgramMax - overhead > 16384) /* max 2-byte VLQ */
    return (0);
  return (ctx->dgramMax - overhead);
}

unsigned int
chanBlbChnRsecMax(
  const struct chanBlbChnRsecCtx *ctx
 ,unsigned char m
){
  unsigned int overhead;

  overhead = ctx->tagSize + ctx->hmacSize + 8;
  if (ctx->dgramMax <= overhead
   || ctx->dgramMax - overhead > 16384) /* max 2-byte VLQ */
    return (0);
  return ((256 - (unsigned int)m) * (ctx->dgramMax - overhead));
}

/* 1-byte hash for cheap stray packet rejection */
static unsigned char
smallHash(
  const unsigned char *d
 ,unsigned int l
){
  unsigned char h;

  h = 0xff;
  while (l--) {
    h ^= *d++;
    h = (unsigned char)((h << 1) | (h >> 7));
  }
  return (h);
}

struct egrEntry {
  unsigned char *work;  /* NULL = empty slot */
  unsigned int stride;
  unsigned int km;
  unsigned int sent;    /* shards sent so far */
  unsigned char delayMs;/* per-message inter-shard delay */
};

struct shardItem {
  struct timespec ts;   /* scheduled send time */
  unsigned int entry;   /* index into egress table */
  unsigned int shard;   /* shard index within work buffer */
};

static void
shardInsert(
  struct shardItem *base
 ,unsigned long nel
 ,const struct shardItem *item
){
  unsigned long lo;
  unsigned long hi;
  unsigned long mid;

  lo = 0;
  hi = nel;
  while (lo < hi) {
    mid = lo + (hi - lo) / 2;
    if (item->ts.tv_sec < base[mid].ts.tv_sec
     || (item->ts.tv_sec == base[mid].ts.tv_sec
      && item->ts.tv_nsec < base[mid].ts.tv_nsec))
      hi = mid;
    else
      lo = mid + 1;
  }
  if (lo < nel)
    memmove(base + lo + 1, base + lo, (nel - lo) * sizeof (*base));
  base[lo] = *item;
}

/*
 * Pack and send one datagram starting from the head of the shard schedule.
 * Packs additional due shards from different messages to the same address.
 * *maxDelayMs (out): largest delay_ms among packed shards.
 * Returns 0 on output failure.
 */
static int
egrSendPack(
  struct chanBlbEgrCtx *v
 ,struct chanBlbChnRsecCtx *ctx
 ,struct egrEntry *table
 ,unsigned int tableSize
 ,unsigned char *packBuf
 ,unsigned char *packSeen
 ,unsigned int dgramMax
 ,struct shardItem *shards
 ,unsigned long *shardCount
 ,unsigned char *maxDelayMs
){
  unsigned long si2;
  unsigned int ei;
  unsigned int si;
  unsigned int wp;
  unsigned int packPos;
  unsigned int fragLen;
  unsigned int ti;
  struct timespec now;

  ei = shards[0].entry;
  si = shards[0].shard;

  /* start pack: copy addr prefix */
  wp = 1 + table[ei].work[0];
  memcpy(packBuf, table[ei].work, wp);
  packPos = wp;
  memset(packSeen, 0, tableSize);

  /* add first fragment */
  fragLen = table[ei].stride - wp;
  memcpy(packBuf + packPos,
    table[ei].work + (unsigned long)si * table[ei].stride + wp,
    fragLen);
  packPos += fragLen;
  packSeen[ei] = 1;
  *maxDelayMs = table[ei].delayMs;

  /* pop head */
  --(*shardCount);
  if (*shardCount > 0)
    memmove(shards, shards + 1, *shardCount * sizeof (struct shardItem));
  ++ctx->egrFrg;
  ++table[ei].sent;

  /* scan remaining due shards for packing */
  clock_gettime(CLOCK_MONOTONIC, &now);
  si2 = 0;
  while (si2 < *shardCount
   && (shards[si2].ts.tv_sec < now.tv_sec
    || (shards[si2].ts.tv_sec == now.tv_sec
     && shards[si2].ts.tv_nsec <= now.tv_nsec))) {
    unsigned int ei2;
    unsigned int wp2;
    unsigned int fragLen2;

    ei2 = shards[si2].entry;
    wp2 = 1 + table[ei2].work[0];
    fragLen2 = table[ei2].stride - wp2;

    /* same address, not same message, fits */
    if (wp2 == wp
     && memcmp(table[ei2].work, packBuf, wp) == 0
     && !packSeen[ei2]
     && packPos + fragLen2 + 1 <= wp + dgramMax) {
      memcpy(packBuf + packPos,
        table[ei2].work + (unsigned long)shards[si2].shard * table[ei2].stride + wp2,
        fragLen2);
      packPos += fragLen2;
      packSeen[ei2] = 1;
      if (table[ei2].delayMs > *maxDelayMs)
        *maxDelayMs = table[ei2].delayMs;
      ++ctx->egrFrg;
      ++table[ei2].sent;
      /* pop this item */
      --(*shardCount);
      if (si2 < *shardCount)
        memmove(shards + si2, shards + si2 + 1,
          (*shardCount - si2) * sizeof (struct shardItem));
      /* don't increment si2: next item slid into this position */
    } else {
      ++si2;
    }
  }

  /* append smallhash (per-datagram) */
  packBuf[packPos] = smallHash(packBuf + wp, packPos - wp);
  ++packPos;

  if (!v->out(v->outCtx, packBuf, packPos))
    return (0);

  /* check all packed entries for completion */
  for (ti = 0; ti < tableSize; ++ti) {
    if (packSeen[ti] && table[ti].work
     && table[ti].sent >= table[ti].km) {
      ++ctx->egrMsg;
      v->free(table[ti].work);
      table[ti].work = 0;
    }
  }
  return (1);
}

/*
 * Wire datagram (packed): [addrlen(1)][addr][frag1][frag2]...[fragN][smallhash(1)]
 * Each fragment: [tag(tagSize)][k-1(1)][m(1)][si(1)][pad_vlq(1-2)][shard_len_vlq(1-2)][shard_data(shard_len)][hmac(hmacSize)]
 * Work buffer per shard slot (no smallhash): [addrlen][addr][tag][k-1][m][si][pad_vlq][shard_len_vlq][shard_data(msgShardSize)][hmac]
 */
void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
){
  struct chanBlbChnRsecCtx *ctx;
  struct egrEntry *table;
  struct shardItem *shards;
  unsigned char *packBuf;
  unsigned char *packSeen;
  chanBlb_t *pending;
  chanArr_t p[1];
  unsigned int tagSize;
  unsigned int hmacSize;
  unsigned int tableSize;
  unsigned int dgramMax;
  unsigned int overhead;
  unsigned int maxShardSize;
  unsigned long shardCount;
  unsigned long shardAlloc;
  unsigned int ti;
  /* Inter-packet pacing state.  After emitting, the next emit
   * must wait at least lastMaxD ms — the largest delay_ms among
   * shards packed into that packet.  Per-message scheduling
   * (shards[].ts) is unchanged so same-destination shards whose
   * timestamps coincide still pack into one datagram. */
  struct timespec lastEmit;
  unsigned char lastMaxD;
  unsigned char hasEmit;

  ctx = (struct chanBlbChnRsecCtx *)v->frmCtx;
  tagSize = ctx->tagSize;
  hmacSize = ctx->hmacSize;
  tableSize = ctx->tableSize;
  dgramMax = ctx->dgramMax;
  overhead = tagSize + hmacSize + 8;
  if (dgramMax <= overhead || dgramMax - overhead > 16384 || !tableSize) {
    v->fin(v);
    return (0);
  }
  maxShardSize = dgramMax - overhead;

  table = (struct egrEntry *)v->realloc(0,
    (unsigned long)tableSize * sizeof (struct egrEntry));
  if (!table) {
    v->fin(v);
    return (0);
  }
  for (ti = 0; ti < tableSize; ++ti)
    table[ti].work = 0;

  /* pack buffer: addr prefix (1+255 max) + wire payload (dgramMax) */
  packBuf = (unsigned char *)v->realloc(0, 1 + 255 + dgramMax);
  if (!packBuf) {
    v->free(table);
    v->fin(v);
    return (0);
  }
  /* same-message avoidance: one byte per table entry */
  packSeen = (unsigned char *)v->realloc(0, tableSize);
  if (!packSeen) {
    v->free(packBuf);
    v->free(table);
    v->fin(v);
    return (0);
  }

  shards = 0;
  shardCount = 0;
  shardAlloc = 0;
  pending = 0;
  lastMaxD = 0;
  hasEmit = 0;

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&pending;
  p[0].o = chanOpGet;

  for (;;) {
    long ns;
    struct timespec now;
    struct timespec target;

    /* Emission target: the later of the head shard's scheduled
     * ts (per-message pacing) and lastEmit + lastMaxD (inter-
     * packet wire pacing).  target is only valid when shardCount
     * is non-zero; reused below after chanOne to gate emission. */
    if (!shardCount) {
      ns = 0; /* block forever */
    } else {
      target = shards[0].ts;
      if (hasEmit) {
        struct timespec nextAllowed;

        nextAllowed = lastEmit;
        nextAllowed.tv_nsec += (long)lastMaxD * 1000000L;
        if (nextAllowed.tv_nsec >= 1000000000L) {
          nextAllowed.tv_sec++;
          nextAllowed.tv_nsec -= 1000000000L;
        }
        if (nextAllowed.tv_sec > target.tv_sec
         || (nextAllowed.tv_sec == target.tv_sec
          && nextAllowed.tv_nsec > target.tv_nsec))
          target = nextAllowed;
      }
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (target.tv_sec < now.tv_sec
       || (target.tv_sec == now.tv_sec
        && target.tv_nsec <= now.tv_nsec)) {
        ns = -1; /* due now, poll */
      } else {
        time_t dsec;

        dsec = target.tv_sec - now.tv_sec;
        if (dsec > 1)
          ns = 1000000000L; /* cap at 1s for ILP32 portability */
        else {
          ns = dsec * 1000000000L
             + (target.tv_nsec - now.tv_nsec);
          if (ns <= 0) ns = 1;
        }
      }
    }

    if (!chanOne(ns, sizeof (p) / sizeof (p[0]), p))
      break;

    if (p[0].s == chanOsGet) {
      /* pending now holds the blob; pause gets, monitor shutdown */
      p[0].v = 0;
      p[0].o = chanOpSht;
    } else if (p[0].s != chanOsTmo) {
      break; /* shutdown */
    }

    /* Emit at most one packet per pass.  shardCount>0 here implies
     * target was computed above; pending processing only runs
     * afterward so shardCount cannot grow between then and now. */
    if (shardCount) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (target.tv_sec < now.tv_sec
       || (target.tv_sec == now.tv_sec
        && target.tv_nsec <= now.tv_nsec)) {
        unsigned char packMaxD;

        if (!egrSendPack(v, ctx, table, tableSize, packBuf, packSeen,
                         dgramMax, shards, &shardCount, &packMaxD))
          goto exit;
        clock_gettime(CLOCK_MONOTONIC, &lastEmit);
        lastMaxD = packMaxD;
        hasEmit = 1;
      }
    }

    /* process pending blob if slot available */
    if (pending) {
      unsigned int slot;

      slot = tableSize;
      for (ti = 0; ti < tableSize; ++ti) {
        if (!table[ti].work) {
          slot = ti;
          break;
        }
      }

      if (slot < tableSize) {
        unsigned int addrlen;
        unsigned int prefixSize;
        unsigned int payloadLen;
        unsigned int k;
        unsigned int km;
        unsigned int msgShardSize;
        unsigned int padding;
        unsigned int padVlqLen;
        unsigned int shardVlqLen;
        unsigned int headerGap;
        unsigned int stride;
        unsigned int i;
        unsigned char mVal;
        unsigned char delayMs;
        unsigned char padVlq[2];
        unsigned char shardVlq[2];
        unsigned char *work;
        const unsigned char **dataPtrs;
        unsigned char **parityPtrs;
        struct shardItem item;
        struct shardItem *tmp;

        /* parse blob prefix: [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_ms(1)][payload] */
        if (pending->l < 1) {
          /* malformed blob; drop and keep running */
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }
        addrlen = pending->b[0];
        prefixSize = 1 + addrlen + tagSize;
        if (pending->l < prefixSize + 2) {
          /* malformed blob; drop and keep running */
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }
        mVal = pending->b[prefixSize];
        delayMs = pending->b[prefixSize + 1];
        payloadLen = pending->l - (prefixSize + 2);

        /* compute k using maxShardSize */
        k = payloadLen > 0 ? (payloadLen + maxShardSize - 1) / maxShardSize : 1;
        if (k > 256) {
          /* payload exceeds chanBlbChnRsecMax(ctx, 0); drop and keep running */
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }
        km = k + (unsigned int)mVal;
        if (km > 256) {
          mVal = (unsigned char)(256 - k);
          km = 256;
        }

        /* per-message shard size: tighter than maxShardSize */
        msgShardSize = payloadLen > 0 ? (payloadLen + k - 1) / k : 0;

        /* padding */
        padding = k * msgShardSize - payloadLen;
        if (padding < 128) {
          padVlq[0] = (unsigned char)padding;
          padVlqLen = 1;
        } else {
          padVlq[0] = (unsigned char)(0x80 | ((padding - 1) >> 7));
          padVlq[1] = (unsigned char)((padding - 1) & 0x7f);
          padVlqLen = 2;
        }

        /* shard_len VLQ encode */
        if (msgShardSize < 128) {
          shardVlq[0] = (unsigned char)msgShardSize;
          shardVlqLen = 1;
        } else {
          shardVlq[0] = (unsigned char)(0x80 | ((msgShardSize - 1) >> 7));
          shardVlq[1] = (unsigned char)((msgShardSize - 1) & 0x7f);
          shardVlqLen = 2;
        }

        /* work slot layout: [addrlen][addr][tag][k-1][m][si][pad_vlq][shard_len_vlq][shard_data(msgShardSize)][hmac] */
        headerGap = prefixSize + 3 + padVlqLen + shardVlqLen;
        stride = headerGap + msgShardSize + hmacSize;

        /* allocate pointer arrays separately from the byte work buffer so
         * the pointer arrays are suitably aligned regardless of stride. */
        dataPtrs = (const unsigned char **)v->realloc(0,
          (unsigned long)k * sizeof (const unsigned char *)
          + (unsigned long)(unsigned int)mVal * sizeof (unsigned char *));
        if (!dataPtrs) {
          /* drop blob and keep running */
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }
        parityPtrs = (unsigned char **)(dataPtrs + k);

        work = (unsigned char *)v->realloc(0, (unsigned long)km * stride);
        if (!work) {
          v->free(dataPtrs);
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }

        /* build header template in first slot */
        memcpy(work, pending->b, prefixSize); /* [addrlen][addr][tag] common prefix */
        work[prefixSize] = (unsigned char)(k - 1);       /* k-1 on wire */
        work[prefixSize + 1] = mVal;                     /* m */
        work[prefixSize + 2] = 0;                        /* shard_index placeholder */
        memcpy(work + prefixSize + 3, padVlq, padVlqLen);
        memcpy(work + prefixSize + 3 + padVlqLen, shardVlq, shardVlqLen);

        /* replicate header into all slots, set shard_index, copy payload */
        for (i = 0; i < km; ++i) {
          unsigned char *s;

          s = work + (unsigned long)i * stride;
          if (i > 0)
            memcpy(s, work, headerGap);
          s[prefixSize + 2] = (unsigned char)i;

          if (i < k) {
            unsigned int off;
            unsigned int len;

            off = i * msgShardSize;
            len = payloadLen - off;
            if (len > msgShardSize)
              len = msgShardSize;
            if (len > 0)
              memcpy(s + headerGap, pending->b + prefixSize + 2 + off, len);
            if (len < msgShardSize)
              memset(s + headerGap + len, 0, msgShardSize - len);
            dataPtrs[i] = s + headerGap;
          } else {
            parityPtrs[i - k] = s + headerGap;
          }
        }

        /* encode parity */
        if (mVal > 0 && msgShardSize > 0
         && rsecEncode(dataPtrs, parityPtrs, msgShardSize, k, (unsigned int)mVal)) {
          /* unexpected encode failure: drop and keep running */
          v->free(dataPtrs);
          v->free(work);
          v->free(pending);
          pending = 0;
          p[0].v = (void **)&pending;
          p[0].o = chanOpGet;
          continue;
        }
        /* pointer arrays no longer needed after encode */
        v->free(dataPtrs);

        /* encrypt each shard (before HMAC: encrypt-then-MAC) */
        /* last data shard (k-1) excludes padding to avoid encrypting known zeros */
        if (ctx->encrypt) {
          for (i = 0; i < km; ++i)
            ctx->encrypt(ctx->cryptCtx, work + (unsigned long)i * stride,
              work + (unsigned long)i * stride + headerGap,
              i == k - 1 && padding > 0 ? msgShardSize - padding : msgShardSize);
        }

        /* sign each slot */
        for (i = 0; i < km; ++i) {
          unsigned char *s;
          unsigned int wp;

          s = work + (unsigned long)i * stride;
          wp = 1 + addrlen;
          /* HMAC covers wire payload: tag through shard_data */
          if (hmacSize > 0)
            ctx->hmacSign(ctx->hmacCtx, s,
              s + headerGap + msgShardSize,
              s + wp, headerGap + msgShardSize - wp);
        }

        v->free(pending);
        pending = 0;

        /* store in table */
        table[slot].work = work;
        table[slot].stride = stride;
        table[slot].km = km;
        table[slot].sent = 0;
        table[slot].delayMs = delayMs;

        /* grow shard schedule if needed */
        if (shardCount + km > shardAlloc) {
          shardAlloc = shardCount + km;
          tmp = (struct shardItem *)v->realloc(shards,
            shardAlloc * sizeof (struct shardItem));
          if (!tmp) {
            /* drop this message and keep running (pending already freed) */
            v->free(table[slot].work);
            table[slot].work = 0;
            p[0].v = (void **)&pending;
            p[0].o = chanOpGet;
            continue;
          }
          shards = tmp;
        }

        /* insert km shard items into schedule */
        clock_gettime(CLOCK_MONOTONIC, &now);
        item.entry = slot;
        item.ts = now;
        for (i = 0; i < km; ++i) {
          item.ts.tv_nsec += (long)delayMs * 1000000L;
          if (item.ts.tv_nsec >= 1000000000L) {
            item.ts.tv_sec++;
            item.ts.tv_nsec -= 1000000000L;
          }
          item.shard = i;
          shardInsert(shards, shardCount, &item);
          ++shardCount;
        }

        /* resume gets */
        p[0].v = (void **)&pending;
        p[0].o = chanOpGet;
      }
    }
  }

exit:
  v->free(pending);
  for (ti = 0; ti < tableSize; ++ti) {
    if (table[ti].work)
      v->free(table[ti].work);
  }
  v->free(shards);
  v->free(packSeen);
  v->free(packBuf);
  v->free(table);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}

struct igrEntry {
  chanBlb_t *blob;
  unsigned char *parity;   /* m * shardSize bytes */
  unsigned char *tag;      /* tagSize bytes, points into allocation tail */
  unsigned int k;
  unsigned int m;
  unsigned int shardSize;
  unsigned int padding;
  unsigned int received;
  unsigned int age;
  unsigned int prefixSize; /* 1 + addrlen + tagSize + 2 (includes rm + um) */
  unsigned char present[256];
};

/*
 * Wire datagram (packed): [addrlen(1)][addr][frag1][frag2]...[fragN][smallhash(1)]
 * Each fragment: [tag(tagSize)][k-1(1)][m(1)][si(1)][pad_vlq(1-2)][shard_len_vlq(1-2)][shard_data(shard_len)][hmac(hmacSize)]
 */
void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
){
  struct chanBlbChnRsecCtx *ctx;
  struct igrEntry **table;
  unsigned char *buf;
  unsigned char *hdr;
  chanArr_t p[1];
  unsigned int tagSize;
  unsigned int tableSize;
  unsigned int hmacSize;
  unsigned int bufSize;
  unsigned int tableUsed;
  unsigned int age;
  unsigned int ti;

  ctx = (struct chanBlbChnRsecCtx *)v->frmCtx;
  tagSize = ctx->tagSize;
  tableSize = ctx->tableSize;
  hmacSize = ctx->hmacSize;

  /* allocate collection table (sorted by tag, compact) */
  table = (struct igrEntry **)v->realloc(0,
    (unsigned long)tableSize * sizeof (struct igrEntry *));
  if (!table) {
    v->fin(v);
    return (0);
  }

  /* allocate receive buffer: full address range (addrlen up to 255) */
  bufSize = 1 + 255 + ctx->dgramMax;
  buf = (unsigned char *)v->realloc(0, bufSize);
  if (!buf) {
    v->free(table);
    v->fin(v);
    return (0);
  }

  /* allocate hdr reconstruction buffer for HMAC/crypto callbacks */
  hdr = (unsigned char *)v->realloc(0, (unsigned long)(1 + 255) + tagSize + 3);
  if (!hdr) {
    v->free(buf);
    v->free(table);
    v->fin(v);
    return (0);
  }

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = 0;
  p[0].o = chanOpPut;
  tableUsed = 0;
  age = 0;

  for (;;) {
    unsigned int n;
    unsigned int addrlen;
    unsigned int wp;
    unsigned int pos;

    /* read one datagram */
    n = v->blb ? chanBlbIgrBlb(v->free, &v->blb, buf, bufSize)
               : v->inp(v->inpCtx, buf, bufSize);
    if (!n)
      break;
    addrlen = buf[0];
    wp = 1 + addrlen;

    /* need at least addr prefix + one minimal fragment + smallhash */
    if (n < wp + tagSize + 3 + 1 + 1 + hmacSize + 1)
      continue;

    /* validate datagram-level smallhash at end */
    if (smallHash(buf + wp, n - wp - 1) != buf[n - 1]) {
      ++ctx->igrHash;
      continue;
    }
    n -= 1; /* strip smallhash */

    /* parse fragments within the datagram */
    pos = wp;
    while (pos + tagSize + 3 + 1 + 1 + hmacSize <= n) {
      unsigned int k;
      unsigned int mVal;
      unsigned int shardIdx;
      unsigned int padding;
      unsigned int padVlqLen;
      unsigned int fragShardSize;
      unsigned int shardVlqLen;
      unsigned int headerLen;
      unsigned int fragEnd;
      struct igrEntry *ent;
      int slot;
      int decoded;
      int found;

      ++ctx->igrFrg;

      /* parse fragment header */
      k = (unsigned int)buf[pos + tagSize] + 1;
      mVal = buf[pos + tagSize + 1];
      shardIdx = buf[pos + tagSize + 2];

      if (k + mVal > 256 || shardIdx >= k + mVal) {
        break; /* malformed: stop parsing this datagram */
      }

      /* decode padding VLQ */
      if (buf[pos + tagSize + 3] & 0x80) {
        if (pos + tagSize + 5 > n)
          break;
        padding = (unsigned int)(buf[pos + tagSize + 3] & 0x7f);
        padding = ((padding << 7) | (unsigned int)(buf[pos + tagSize + 4] & 0x7f)) + 1;
        padVlqLen = 2;
      } else {
        padding = buf[pos + tagSize + 3];
        padVlqLen = 1;
      }

      /* decode shard_len VLQ */
      {
        unsigned int vlqOff;

        vlqOff = pos + tagSize + 3 + padVlqLen;
        if (vlqOff >= n)
          break;
        if (buf[vlqOff] & 0x80) {
          if (vlqOff + 2 > n)
            break;
          fragShardSize = (unsigned int)(buf[vlqOff] & 0x7f);
          fragShardSize = ((fragShardSize << 7) | (unsigned int)(buf[vlqOff + 1] & 0x7f)) + 1;
          shardVlqLen = 2;
        } else {
          fragShardSize = buf[vlqOff];
          shardVlqLen = 1;
        }
      }

      headerLen = tagSize + 3 + padVlqLen + shardVlqLen;
      fragEnd = pos + headerLen + fragShardSize + hmacSize;
      if (fragEnd > n)
        break; /* fragment extends past datagram */

      /* validate padding against total data capacity */
      if (padding > k * fragShardSize) {
        pos = fragEnd;
        continue;
      }

      /* reconstruct hdr: [addrlen][addr][tag][k-1][m][si] */
      memcpy(hdr, buf, wp); /* addr prefix from datagram */
      memcpy(hdr + wp, buf + pos, tagSize + 3); /* tag + k-1 + m + si */

      /* validate HMAC if enabled */
      if (hmacSize > 0) {
        if (!ctx->hmacVrfy(ctx->hmacCtx, hdr,
                           buf + pos + headerLen + fragShardSize,
                           buf + pos, headerLen + fragShardSize)) {
          ++ctx->igrHmac;
          pos = fragEnd;
          continue;
        }
      }

      /* decrypt shard data (after HMAC verify: MAC-then-decrypt) */
      /* last data shard (k-1) excludes padding to match egress */
      if (ctx->decrypt)
        ctx->decrypt(ctx->cryptCtx, hdr, buf + pos + headerLen,
          shardIdx == k - 1 && padding > 0 ? fragShardSize - padding : fragShardSize);

      ++age;
      /* halve all ages to prevent wraparound on 32-bit */
      if (age >= (unsigned int)-1 / 2) {
        for (ti = 0; ti < tableUsed; ++ti)
          table[ti]->age >>= 1;
        age >>= 1;
      }

      /* binary search for tag in sorted table */
      {
        unsigned int lo;
        unsigned int hi;
        unsigned int mid;
        int c;

        lo = 0;
        hi = tableUsed;
        found = 0;
        while (lo < hi) {
          mid = lo + (hi - lo) / 2;
          c = memcmp(buf + pos, table[mid]->tag, tagSize);
          if (c < 0)
            hi = mid;
          else if (c > 0)
            lo = mid + 1;
          else {
            found = 1;
            lo = mid;
            break;
          }
        }
        slot = (int)lo;
      }

      /* handle found entry with parameter mismatch */
      if (found) {
        if (table[slot]->k != k || table[slot]->m != mVal
         || table[slot]->shardSize != fragShardSize) {
          /* mismatch: evict — deliver loss notification */
          if (table[slot]->blob) {
            chanBlb_t *lb;

            ++ctx->igrEvict;
            lb = table[slot]->blob;
            lb->l = table[slot]->prefixSize;
            lb->b[table[slot]->prefixSize - 2] = 0;
            lb->b[table[slot]->prefixSize - 1] =
              (unsigned char)table[slot]->received;
            p[0].v = (void **)&lb;
            if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1
             || p[0].s != chanOsPut)
              v->free(lb);
          }
          v->free(table[slot]);
          --tableUsed;
          if ((unsigned int)slot < tableUsed)
            memmove(table + slot, table + slot + 1,
              (unsigned long)(tableUsed - (unsigned int)slot) * sizeof (*table));
          found = 0;
          /* slot still holds the correct insert position */
        }
      }

      /* if not found, create entry and insert at sorted position */
      if (!found) {
        unsigned int insertPos;

        insertPos = (unsigned int)slot;

        /* evict LRU if table full */
        if (tableUsed >= tableSize) {
          unsigned int lruIdx;
          unsigned int minAge;

          lruIdx = 0;
          minAge = table[0]->age;
          for (ti = 1; ti < tableUsed; ++ti) {
            if (table[ti]->age < minAge) {
              minAge = table[ti]->age;
              lruIdx = ti;
            }
          }
          /* LRU evict — deliver loss notification */
          if (table[lruIdx]->blob) {
            chanBlb_t *lb;

            ++ctx->igrEvict;
            lb = table[lruIdx]->blob;
            lb->l = table[lruIdx]->prefixSize;
            lb->b[table[lruIdx]->prefixSize - 2] = 0;
            lb->b[table[lruIdx]->prefixSize - 1] =
              (unsigned char)table[lruIdx]->received;
            p[0].v = (void **)&lb;
            if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1
             || p[0].s != chanOsPut)
              v->free(lb);
          }
          v->free(table[lruIdx]);
          --tableUsed;
          if (lruIdx < tableUsed)
            memmove(table + lruIdx, table + lruIdx + 1,
              (unsigned long)(tableUsed - lruIdx) * sizeof (*table));
          if (lruIdx < insertPos)
            --insertPos;
        }

        /* create new entry: struct + parity storage + tag copy */
        ent = (struct igrEntry *)v->realloc(0,
          sizeof (struct igrEntry)
          + (unsigned long)mVal * fragShardSize + tagSize);
        if (!ent)
          break;
        ent->blob = 0;
        ent->parity = (unsigned char *)ent + sizeof (struct igrEntry);
        ent->tag = ent->parity + (unsigned long)mVal * fragShardSize;
        ent->k = k;
        ent->m = mVal;
        ent->shardSize = fragShardSize;
        ent->padding = padding;
        ent->received = 0;
        ent->age = age;
        ent->prefixSize = 1 + addrlen + tagSize + 2;
        memcpy(ent->tag, buf + pos, tagSize);
        memset(ent->present, 0, sizeof (ent->present));

        /* allocate output blob */
        ent->blob = (chanBlb_t *)v->realloc(0,
          chanBlb_tSize(ent->prefixSize + k * fragShardSize));
        if (!ent->blob) {
          v->free(ent);
          break;
        }
        /* write blob prefix: [addrlen][addr][tag][rm][um] */
        memcpy(ent->blob->b, buf, 1 + addrlen);
        memcpy(ent->blob->b + 1 + addrlen, buf + pos, tagSize);
        ent->blob->b[1 + addrlen + tagSize] = 0;
        ent->blob->b[1 + addrlen + tagSize + 1] = 0;

        /* insert at sorted position */
        if (insertPos < tableUsed)
          memmove(table + insertPos + 1, table + insertPos,
            (unsigned long)(tableUsed - insertPos) * sizeof (*table));
        table[insertPos] = ent;
        ++tableUsed;
        slot = (int)insertPos;
      }

      ent = table[slot];

      /* already delivered: ignore late fragments */
      if (!ent->blob) {
        ++ctx->igrLate;
        pos = fragEnd;
        continue;
      }

      /* ignore duplicate shard */
      if (ent->present[shardIdx]) {
        ++ctx->igrDup;
        pos = fragEnd;
        continue;
      }

      /* store shard */
      if (shardIdx < k) {
        memcpy(ent->blob->b + ent->prefixSize
          + (unsigned long)shardIdx * ent->shardSize,
          buf + pos + headerLen, fragShardSize);
      } else {
        memcpy(ent->parity
          + (unsigned long)(shardIdx - k) * ent->shardSize,
          buf + pos + headerLen, fragShardSize);
      }
      ent->present[shardIdx] = 1;
      ent->received++;
      ent->age = age;

      pos = fragEnd;

      /* not enough shards yet */
      if (ent->received < k)
        continue;

      decoded = 0;

      /* reassemble: check if all data shards present */
      {
        unsigned int dataPresent;

        dataPresent = 0;
        for (ti = 0; ti < k; ++ti) {
          if (ent->present[ti])
            ++dataPresent;
        }

        if (dataPresent == k) {
          /* all data shards present, payload already in place */
          ent->blob->l = ent->prefixSize + k * ent->shardSize - ent->padding;
        } else {
          /* need RSEC decode */
          unsigned long wsSize;
          unsigned char *workspace;
          const unsigned char **recvPtrs;
          unsigned char **outPtrs;
          unsigned char *indices;
          unsigned char *workArea;
          unsigned char *shardCopies;
          unsigned int ri;
          unsigned int ci;

          wsSize = (unsigned long)k * sizeof (const unsigned char *)
                 + (unsigned long)k * sizeof (unsigned char *)
                 + k + RS_WORK_SIZE(k)
                 + (unsigned long)dataPresent * ent->shardSize;
          workspace = (unsigned char *)v->realloc(0, wsSize);
          if (!workspace) {
            v->free(ent->blob);
            v->free(ent);
            --tableUsed;
            if ((unsigned int)slot < tableUsed)
              memmove(table + slot, table + slot + 1,
                (unsigned long)(tableUsed - (unsigned int)slot) * sizeof (*table));
            continue;
          }
          recvPtrs = (const unsigned char **)workspace;
          outPtrs = (unsigned char **)((unsigned char *)recvPtrs
            + (unsigned long)k * sizeof (const unsigned char *));
          indices = (unsigned char *)outPtrs
            + (unsigned long)k * sizeof (unsigned char *);
          workArea = indices + k;
          shardCopies = workArea + RS_WORK_SIZE(k);

          /* collect any k received shards */
          /* copy present data shards to avoid aliasing with outPtrs */
          ri = 0;
          ci = 0;
          for (ti = 0; ti < k && ri < k; ++ti) {
            if (ent->present[ti]) {
              memcpy(shardCopies + (unsigned long)ci * ent->shardSize,
                ent->blob->b + ent->prefixSize
                  + (unsigned long)ti * ent->shardSize,
                ent->shardSize);
              recvPtrs[ri] = shardCopies
                + (unsigned long)ci * ent->shardSize;
              indices[ri] = (unsigned char)ti;
              ++ri;
              ++ci;
            }
          }
          for (ti = k; ti < k + ent->m && ri < k; ++ti) {
            if (ent->present[ti]) {
              recvPtrs[ri] = ent->parity
                + (unsigned long)(ti - k) * ent->shardSize;
              indices[ri] = (unsigned char)ti;
              ++ri;
            }
          }

          /* output pointers: decoded data lands directly in blob */
          for (ti = 0; ti < k; ++ti)
            outPtrs[ti] = ent->blob->b + ent->prefixSize
              + (unsigned long)ti * ent->shardSize;

          if (rsecDecode(recvPtrs, indices, outPtrs,
                         ent->shardSize, k, ent->m, workArea)) {
            v->free(workspace);
            v->free(ent->blob);
            v->free(ent);
            --tableUsed;
            if ((unsigned int)slot < tableUsed)
              memmove(table + slot, table + slot + 1,
                (unsigned long)(tableUsed - (unsigned int)slot) * sizeof (*table));
            continue;
          }
          v->free(workspace);
          ent->blob->l = ent->prefixSize + k * ent->shardSize - ent->padding;
          decoded = 1;
        }
      }

      /* rm = sender's m, um = parity shards used (missing data shards) */
      {
        unsigned int dp;

        dp = 0;
        for (ti = 0; ti < k; ++ti)
          dp += ent->present[ti] ? 1 : 0;
        ent->blob->b[ent->prefixSize - 2] = (unsigned char)ent->m;
        ent->blob->b[ent->prefixSize - 1] = (unsigned char)(ent->k - dp);
      }

      /* put blob on channel */
      p[0].v = (void **)&ent->blob;
      if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1
       || p[0].s != chanOsPut) {
        v->free(ent->blob);
        v->free(ent);
        --tableUsed;
        if ((unsigned int)slot < tableUsed)
          memmove(table + slot, table + slot + 1,
            (unsigned long)(tableUsed - (unsigned int)slot) * sizeof (*table));
        goto done;
      }
      /* blob ownership transferred; keep entry for de-dup of late fragments */
      ++ctx->igrMsg;
      if (decoded)
        ++ctx->igrDcd;
      ent->blob = 0;
    }
  }

done:
  /* cleanup: free remaining entries and their blobs */
  for (ti = 0; ti < tableUsed; ++ti) {
    if (table[ti]->blob)
      v->free(table[ti]->blob);
    v->free(table[ti]);
  }
  v->free(hdr);
  v->free(buf);
  v->free(table);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
