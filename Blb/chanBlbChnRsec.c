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

  overhead = ctx->tagSize + ctx->hmacSize + 6;
  if (ctx->dgramMax <= overhead)
    return (0);
  return (ctx->dgramMax - overhead);
}

unsigned int
chanBlbChnRsecMax(
  const struct chanBlbChnRsecCtx *ctx
 ,unsigned char m
){
  unsigned int overhead;

  overhead = ctx->tagSize + ctx->hmacSize + 6;
  if (ctx->dgramMax <= overhead)
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
  unsigned int age;     /* for LRU eviction */
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

static unsigned long
shardEvict(
  struct shardItem *base
 ,unsigned long nel
 ,unsigned int entry
){
  unsigned long r;
  unsigned long w;

  for (r = w = 0; r < nel; ++r) {
    if (base[r].entry != entry) {
      if (w != r)
        base[w] = base[r];
      ++w;
    }
  }
  return (w);
}

/* Wire fragment: [addrlen(1)][addr(addrlen)][tag(tagSize)][k-1(1)][m(1)][shard_index(1)][padding_vlq(1-2)][shard_data(shardSize)][hmac(hmacSize)][smallhash(1)] */
void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
){
  struct chanBlbChnRsecCtx *ctx;
  struct egrEntry *table;
  struct shardItem *shards;
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int tagSize;
  unsigned int hmacSize;
  unsigned int tableSize;
  unsigned int overhead;
  unsigned int shardSize;
  unsigned int age;
  unsigned long shardCount;
  unsigned long shardAlloc;
  unsigned int ti;

  ctx = (struct chanBlbChnRsecCtx *)v->frmCtx;
  tagSize = ctx->tagSize;
  hmacSize = ctx->hmacSize;
  tableSize = ctx->tableSize;
  overhead = tagSize + hmacSize + 6;
  if (ctx->dgramMax <= overhead || !tableSize) {
    v->fin(v);
    return (0);
  }
  shardSize = ctx->dgramMax - overhead;

  table = (struct egrEntry *)v->realloc(0,
    (unsigned long)tableSize * sizeof (struct egrEntry));
  if (!table) {
    v->fin(v);
    return (0);
  }
  for (ti = 0; ti < tableSize; ++ti)
    table[ti].work = 0;

  shards = 0;
  shardCount = 0;
  shardAlloc = 0;

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  age = 0;

  for (;;) {
    long ns;

    /* compute timeout from head of shard schedule */
    if (!shardCount) {
      ns = 0; /* block forever */
    } else {
      struct timespec now;

      clock_gettime(CLOCK_MONOTONIC, &now);
      if (shards[0].ts.tv_sec < now.tv_sec
       || (shards[0].ts.tv_sec == now.tv_sec
        && shards[0].ts.tv_nsec <= now.tv_nsec)) {
        ns = -1; /* shards due now, poll for blob */
      } else {
        ns = (shards[0].ts.tv_sec - now.tv_sec) * 1000000000L
           + (shards[0].ts.tv_nsec - now.tv_nsec);
        if (ns <= 0) ns = 1;
      }
    }

    if (!chanOne(ns, sizeof (p) / sizeof (p[0]), p))
      break;

    if (p[0].s == chanOsGet) {
      unsigned int addrlen;
      unsigned int prefixSize;
      unsigned int payloadLen;
      unsigned int k;
      unsigned int km;
      unsigned int padding;
      unsigned int vlqLen;
      unsigned int headerGap;
      unsigned int stride;
      unsigned int i;
      unsigned int slot;
      unsigned char mVal;
      unsigned char delayMs;
      unsigned char vlqBuf[2];
      unsigned char *work;
      const unsigned char **dataPtrs;
      unsigned char **parityPtrs;
      struct timespec now;
      struct shardItem item;
      struct shardItem *tmp;

      /* find empty slot */
      slot = tableSize;
      for (ti = 0; ti < tableSize; ++ti) {
        if (!table[ti].work) {
          slot = ti;
          break;
        }
      }

      /* table full: evict LRU */
      if (slot == tableSize) {
        unsigned int minAge;

        minAge = age + 1;
        for (ti = 0; ti < tableSize; ++ti) {
          if (table[ti].work && table[ti].age < minAge) {
            minAge = table[ti].age;
            slot = ti;
          }
        }
        if (slot < tableSize) {
          ctx->egrLost += table[slot].km - table[slot].sent;
          shardCount = shardEvict(shards, shardCount, slot);
          v->free(table[slot].work);
          table[slot].work = 0;
        } else {
          v->free(m);
          continue;
        }
      }

      /* parse blob prefix: [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_ms(1)][payload] */
      addrlen = m->b[0];
      prefixSize = 1 + addrlen + tagSize;
      if (m->l < prefixSize + 2) {
        v->free(m);
        continue;
      }
      mVal = m->b[prefixSize];
      delayMs = m->b[prefixSize + 1];
      payloadLen = m->l - (prefixSize + 2);

      /* compute k */
      k = payloadLen > 0 ? (payloadLen + shardSize - 1) / shardSize : 1;
      km = k + (unsigned int)mVal;
      if (km > 256) {
        v->free(m);
        continue;
      }

      /* padding and VLQ encode */
      padding = k * shardSize - payloadLen;
      if (padding < 128) {
        vlqBuf[0] = (unsigned char)padding;
        vlqLen = 1;
      } else {
        vlqBuf[0] = (unsigned char)(0x80 | ((padding - 1) >> 7));
        vlqBuf[1] = (unsigned char)((padding - 1) & 0x7f);
        vlqLen = 2;
      }

      /* gap layout: [addrlen][addr][tag][k-1][m][shard_index][padding_vlq][shard_data][hmac][smallhash] */
      headerGap = prefixSize + 3 + vlqLen;
      stride = headerGap + shardSize + hmacSize + 1;

      /* allocate: slots + data pointer array + parity pointer array */
      work = (unsigned char *)v->realloc(0,
        (unsigned long)km * stride
        + (unsigned long)k * sizeof (const unsigned char *)
        + (unsigned long)(unsigned int)mVal * sizeof (unsigned char *));
      if (!work) {
        v->free(m);
        break;
      }
      dataPtrs = (const unsigned char **)(work + (unsigned long)km * stride);
      parityPtrs = (unsigned char **)((unsigned char *)dataPtrs
        + (unsigned long)k * sizeof (const unsigned char *));

      /* build header template in first slot */
      memcpy(work, m->b, prefixSize); /* [addrlen][addr][tag] common prefix */
      work[prefixSize] = (unsigned char)(k - 1);       /* k-1 on wire */
      work[prefixSize + 1] = mVal;                     /* m */
      work[prefixSize + 2] = 0;                        /* shard_index placeholder */
      memcpy(work + prefixSize + 3, vlqBuf, vlqLen);   /* padding VLQ */

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

          off = i * shardSize;
          len = payloadLen - off;
          if (len > shardSize)
            len = shardSize;
          if (len > 0)
            memcpy(s + headerGap, m->b + prefixSize + 2 + off, len);
          if (len < shardSize)
            memset(s + headerGap + len, 0, shardSize - len);
          dataPtrs[i] = s + headerGap;
        } else {
          parityPtrs[i - k] = s + headerGap;
        }
      }

      /* encode parity */
      if (mVal > 0)
        rsecEncode(dataPtrs, parityPtrs, shardSize, k, (unsigned int)mVal);

      /* encrypt each shard (before HMAC: encrypt-then-MAC) */
      /* last data shard (k-1) excludes padding to avoid encrypting known zeros */
      if (ctx->encrypt) {
        for (i = 0; i < km; ++i)
          ctx->encrypt(ctx->cryptCtx, work + (unsigned long)i * stride,
            work + (unsigned long)i * stride + headerGap,
            i == k - 1 && padding > 0 ? shardSize - padding : shardSize);
      }

      /* sign and hash each slot */
      for (i = 0; i < km; ++i) {
        unsigned char *s;
        unsigned int wp;

        s = work + (unsigned long)i * stride;
        wp = 1 + addrlen;
        /* HMAC covers wire payload: tag through shard_data */
        if (hmacSize > 0)
          ctx->hmacSign(ctx->hmacCtx, s,
            s + headerGap + shardSize,
            s + wp, headerGap + shardSize - wp);
        /* small hash covers wire payload: tag through hmac */
        s[stride - 1] = smallHash(s + wp, stride - wp - 1);
      }

      v->free(m);

      /* store in table */
      table[slot].work = work;
      table[slot].stride = stride;
      table[slot].km = km;
      table[slot].sent = 0;
      table[slot].age = ++age;

      /* grow shard schedule if needed */
      if (shardCount + km > shardAlloc) {
        shardAlloc = shardCount + km;
        tmp = (struct shardItem *)v->realloc(shards,
          shardAlloc * sizeof (struct shardItem));
        if (!tmp) {
          v->free(table[slot].work);
          table[slot].work = 0;
          break;
        }
        shards = tmp;
      }

      /* insert km shard items into schedule */
      clock_gettime(CLOCK_MONOTONIC, &now);
      item.entry = slot;
      item.ts = now;
      for (i = 0; i < km; ++i) {
        item.shard = i;
        shardInsert(shards, shardCount, &item);
        ++shardCount;
        item.ts.tv_nsec += (long)delayMs * 1000000L;
        if (item.ts.tv_nsec >= 1000000000L) {
          item.ts.tv_sec++;
          item.ts.tv_nsec -= 1000000000L;
        }
      }
    } else if (p[0].s != chanOsTmo) {
      break; /* shutdown */
    }

    /* send all due shards */
    {
      struct timespec now;

      clock_gettime(CLOCK_MONOTONIC, &now);
      while (shardCount > 0
       && (shards[0].ts.tv_sec < now.tv_sec
        || (shards[0].ts.tv_sec == now.tv_sec
         && shards[0].ts.tv_nsec <= now.tv_nsec))) {
        unsigned int ei;
        unsigned int si;

        ei = shards[0].entry;
        si = shards[0].shard;

        /* pop head */
        --shardCount;
        if (shardCount > 0)
          memmove(shards, shards + 1, shardCount * sizeof (struct shardItem));

        if (!v->out(v->outCtx,
          table[ei].work + (unsigned long)si * table[ei].stride,
          table[ei].stride))
          goto exit;
        ++ctx->egrFrg;
        ++table[ei].sent;

        if (table[ei].sent >= table[ei].km) {
          ++ctx->egrMsg;
          v->free(table[ei].work);
          table[ei].work = 0;
        }
      }
    }
  }

exit:
  for (ti = 0; ti < tableSize; ++ti) {
    if (table[ti].work)
      v->free(table[ti].work);
  }
  v->free(shards);
  v->free(table);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}

struct rsecEntry {
  unsigned int k;
  unsigned int m;
  unsigned int shardSize;
  unsigned int padding;
  unsigned int received;
  unsigned int age;
  unsigned int prefixSize; /* 1 + addrlen + tagSize + 1 (includes m byte) */
  chanBlb_t *blob;
  unsigned char *parity;   /* m * shardSize bytes */
  unsigned char *tag;      /* tagSize bytes, points into allocation tail */
  unsigned char present[256];
};

/* Wire fragment: [addrlen(1)][addr(addrlen)][tag(tagSize)][k-1(1)][m(1)][shard_index(1)][padding_vlq(1-2)][shard_data(shardSize)][hmac(hmacSize)][smallhash(1)] */
void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
){
  struct chanBlbChnRsecCtx *ctx;
  struct rsecEntry **table;
  unsigned char *buf;
  chanArr_t p[1];
  unsigned int tagSize;
  unsigned int tableSize;
  unsigned int hmacSize;
  unsigned int bufSize;
  unsigned int age;
  unsigned int ti;

  ctx = (struct chanBlbChnRsecCtx *)v->frmCtx;
  tagSize = ctx->tagSize;
  tableSize = ctx->tableSize;
  hmacSize = ctx->hmacSize;

  /* allocate collection table */
  table = (struct rsecEntry **)v->realloc(0,
    (unsigned long)tableSize * sizeof (struct rsecEntry *));
  if (!table) {
    v->fin(v);
    return (0);
  }
  memset(table, 0, (unsigned long)tableSize * sizeof (struct rsecEntry *));

  /* allocate receive buffer: max datagram size */
  bufSize = 1 + 128 + ctx->dgramMax;
  buf = (unsigned char *)v->realloc(0, bufSize);
  if (!buf) {
    v->free(table);
    v->fin(v);
    return (0);
  }

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = 0;
  p[0].o = chanOpPut;
  age = 0;

  for (;;) {
    unsigned int n;
    unsigned int addrlen;
    unsigned int wp;
    unsigned int prefixSize;
    unsigned int k;
    unsigned int mVal;
    unsigned int shardIdx;
    unsigned int padding;
    unsigned int vlqLen;
    unsigned int headerLen;
    unsigned int fragDataLen;
    struct rsecEntry *ent;
    int slot;
    int decoded;

    /* read one datagram */
    n = v->blb ? chanBlbIgrBlb(v->free, &v->blb, buf, bufSize)
               : v->inp(v->inpCtx, buf, bufSize);
    if (!n)
      break;
    ++ctx->igrFrg;
    addrlen = buf[0];
    /* need at least: addrlen + tagSize + k + m + shard_index + vlq + hmacSize + smallhash */
    if (n < 1 + addrlen + tagSize + 1 + 1 + 1 + 1 + hmacSize + 1)
      continue;

    /* validate small hash: covers wire payload (after addr prefix) except last byte */
    wp = 1 + addrlen;
    if (smallHash(buf + wp, n - wp - 1) != buf[n - 1]) {
      ++ctx->igrHash;
      continue;
    }
    n -= 1; /* strip small hash */

    /* validate HMAC if enabled */
    if (hmacSize > 0) {
      if (!ctx->hmacVrfy(ctx->hmacCtx, buf,
                         buf + n - hmacSize,
                         buf + wp, n - wp - hmacSize)) {
        ++ctx->igrHmac;
        continue;
      }
      n -= hmacSize; /* strip HMAC */
    }

    /* parse fragment header */
    prefixSize = 1 + addrlen + tagSize;
    k = (unsigned int)buf[prefixSize] + 1;
    mVal = buf[prefixSize + 1];
    shardIdx = buf[prefixSize + 2];

    if (k + mVal > 256 || shardIdx >= k + mVal)
      continue;

    /* decode padding VLQ */
    if (buf[prefixSize + 3] & 0x80) {
      padding = (unsigned int)(buf[prefixSize + 3] & 0x7f);
      padding = ((padding << 7) | (unsigned int)buf[prefixSize + 4]) + 1;
      vlqLen = 2;
    } else {
      padding = buf[prefixSize + 3];
      vlqLen = 1;
    }

    headerLen = prefixSize + 3 + vlqLen;
    if (n <= headerLen)
      continue;
    fragDataLen = n - headerLen;

    /* decrypt shard data (after HMAC verify: MAC-then-decrypt) */
    /* last data shard (k-1) excludes padding to match egress */
    if (ctx->decrypt)
      ctx->decrypt(ctx->cryptCtx, buf, buf + headerLen,
        shardIdx == k - 1 && padding > 0 ? fragDataLen - padding : fragDataLen);

    ++age;

    /* lookup tag in table */
    slot = -1;
    for (ti = 0; ti < tableSize; ++ti) {
      if (table[ti]
       && memcmp(table[ti]->tag, buf + 1 + addrlen, tagSize) == 0) {
        if (table[ti]->k != k || table[ti]->m != mVal
         || table[ti]->shardSize != fragDataLen) {
          /* mismatch: evict */
          if (table[ti]->blob) {
            ++ctx->igrLost;
            v->free(table[ti]->blob);
          }
          v->free(table[ti]);
          table[ti] = 0;
        } else {
          slot = (int)ti;
        }
        break;
      }
    }

    /* if not found, find empty slot or evict LRU */
    if (slot < 0) {
      int emptySlot;
      unsigned int minAge;
      int lruSlot;

      emptySlot = -1;
      minAge = age;
      lruSlot = -1;
      for (ti = 0; ti < tableSize; ++ti) {
        if (!table[ti]) {
          emptySlot = (int)ti;
          break;
        }
        if (table[ti]->age < minAge) {
          minAge = table[ti]->age;
          lruSlot = (int)ti;
        }
      }
      if (emptySlot >= 0) {
        slot = emptySlot;
      } else if (lruSlot >= 0) {
        if (table[lruSlot]->blob) {
          ++ctx->igrLost;
          v->free(table[lruSlot]->blob);
        }
        v->free(table[lruSlot]);
        table[lruSlot] = 0;
        slot = lruSlot;
      } else {
        continue;
      }

      /* create new entry: struct + parity storage + tag copy */
      ent = (struct rsecEntry *)v->realloc(0,
        sizeof (struct rsecEntry)
        + (unsigned long)mVal * fragDataLen + tagSize);
      if (!ent)
        break;
      ent->k = k;
      ent->m = mVal;
      ent->shardSize = fragDataLen;
      ent->padding = padding;
      ent->received = 0;
      ent->age = age;
      ent->prefixSize = 1 + addrlen + tagSize + 1;
      ent->parity = (unsigned char *)ent + sizeof (struct rsecEntry);
      ent->tag = ent->parity + (unsigned long)mVal * fragDataLen;
      memcpy(ent->tag, buf + 1 + addrlen, tagSize);
      memset(ent->present, 0, sizeof (ent->present));

      /* allocate output blob */
      ent->blob = (chanBlb_t *)v->realloc(0,
        chanBlb_tSize(ent->prefixSize + k * fragDataLen));
      if (!ent->blob) {
        v->free(ent);
        break;
      }
      /* write blob prefix: [addrlen][addr][tag][m] */
      memcpy(ent->blob->b, buf, 1 + addrlen);
      memcpy(ent->blob->b + 1 + addrlen, buf + 1 + addrlen, tagSize);
      ent->blob->b[1 + addrlen + tagSize] = 0;

      table[slot] = ent;
    }

    ent = table[slot];

    /* already delivered: ignore late fragments */
    if (!ent->blob) {
      ++ctx->igrLate;
      continue;
    }

    /* ignore duplicate shard */
    if (ent->present[shardIdx]) {
      ++ctx->igrDup;
      continue;
    }

    /* store shard */
    if (shardIdx < k) {
      memcpy(ent->blob->b + ent->prefixSize
        + (unsigned long)shardIdx * ent->shardSize,
        buf + headerLen, fragDataLen);
    } else {
      memcpy(ent->parity
        + (unsigned long)(shardIdx - k) * ent->shardSize,
        buf + headerLen, fragDataLen);
    }
    ent->present[shardIdx] = 1;
    ent->received++;
    ent->age = age;

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
        unsigned int ri;

        wsSize = (unsigned long)k * sizeof (const unsigned char *)
               + (unsigned long)k * sizeof (unsigned char *)
               + k + RS_WORK_SIZE(k);
        workspace = (unsigned char *)v->realloc(0, wsSize);
        if (!workspace) {
          v->free(ent->blob);
          v->free(ent);
          table[slot] = 0;
          continue;
        }
        recvPtrs = (const unsigned char **)workspace;
        outPtrs = (unsigned char **)((unsigned char *)recvPtrs
          + (unsigned long)k * sizeof (const unsigned char *));
        indices = (unsigned char *)outPtrs
          + (unsigned long)k * sizeof (unsigned char *);
        workArea = indices + k;

        /* collect any k received shards */
        ri = 0;
        for (ti = 0; ti < k && ri < k; ++ti) {
          if (ent->present[ti]) {
            recvPtrs[ri] = ent->blob->b + ent->prefixSize
              + (unsigned long)ti * ent->shardSize;
            indices[ri] = (unsigned char)ti;
            ++ri;
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
          table[slot] = 0;
          continue;
        }
        v->free(workspace);
        ent->blob->l = ent->prefixSize + k * ent->shardSize - ent->padding;
        decoded = 1;
      }
    }

    /* fraction of sender's m consumed (0-255) */
    {
      unsigned int dp;

      dp = 0;
      for (ti = 0; ti < k; ++ti)
        dp += ent->present[ti] ? 1 : 0;
      ent->blob->b[ent->prefixSize - 1] =
        ent->m > 0
          ? (unsigned char)((ent->k - dp) * 255 / ent->m)
          : 0;
    }

    /* put blob on channel */
    p[0].v = (void **)&ent->blob;
    if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1
     || p[0].s != chanOsPut) {
      v->free(ent->blob);
      v->free(ent);
      table[slot] = 0;
      break;
    }
    /* blob ownership transferred; keep entry for de-dup of late fragments */
    ++ctx->igrMsg;
    if (decoded)
      ++ctx->igrDcd;
    ent->blob = 0;
  }

  /* cleanup: free remaining entries and their blobs */
  for (ti = 0; ti < tableSize; ++ti) {
    if (table[ti]) {
      if (table[ti]->blob)
        v->free(table[ti]->blob);
      v->free(table[ti]);
    }
  }
  v->free(buf);
  v->free(table);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
