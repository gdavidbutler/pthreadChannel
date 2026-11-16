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

/* Generated with Claude Code (https://claude.ai/code) */

#include <string.h>
#include <time.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "rsec.h"
#include "chanBlbChnRsec.h"

/* a after b */
#define TSAFTER(a, b) ((a).tv_sec > (b).tv_sec \
 || ((a).tv_sec == (b).tv_sec && (a).tv_nsec > (b).tv_nsec))

/*
 * A fragment's fixed overhead: [k-1][m][si][pad], a VLQ of at most 2,
 * and the tag and the gate.  A minimal fragment, a 1-byte VLQ and no
 * shard, is one less.
 */
unsigned int
chanBlbChnRsecShard(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int dtgHashSize
){
  unsigned int overhead;

  overhead = tagSize + dtgHashSize + 6;
  if (dgramMax <= overhead
   || dgramMax - overhead > 16384)
    return (0);
  return (dgramMax - overhead);
}

unsigned int
chanBlbChnRsecMax(
  unsigned int dgramMax
 ,unsigned int tagSize
 ,unsigned int dtgHashSize
 ,unsigned char m
){
  unsigned int s;

  if (!(s = chanBlbChnRsecShard(dgramMax, tagSize, dtgHashSize)))
    return (0);
  return ((256 - m) * s);
}

/*
 * The counters behind opaque, the framer's: allocated on its entry,
 * freed at its exit.  The framer counts in its own copy and publishes
 * it whole before each wait.  One mutex for every framer guards each
 * published copy and opaque itself, so a Snap never meets counters
 * freed under it, whenever and however the framer ends.
 */
static pthread_mutex_t Mu = PTHREAD_MUTEX_INITIALIZER;

void
chanBlbChnRsecEgrSnap(
  struct chanBlbChnRsecEgrCtx *ctx
 ,struct chanBlbChnRsecEgrCtrs *out
){
  struct chanBlbChnRsecEgrCtrs *p;

  pthread_mutex_lock(&Mu);
  if ((p = ctx->opaque))
    *out = *p;
  else
    memset(out, 0, sizeof (*out));
  pthread_mutex_unlock(&Mu);
}

void
chanBlbChnRsecIgrSnap(
  struct chanBlbChnRsecIgrCtx *ctx
 ,struct chanBlbChnRsecIgrCtrs *out
){
  struct chanBlbChnRsecIgrCtrs *p;

  pthread_mutex_lock(&Mu);
  if ((p = ctx->opaque))
    *out = *p;
  else
    memset(out, 0, sizeof (*out));
  pthread_mutex_unlock(&Mu);
}

/*
 * Egress.  A slot holds a message's k + m shards, each a fragment ready
 * but for the gate, stride bytes apart:
 *   [addrlen][addr][tag][k-1][m][si][pad][shard_len_vlq][shard_data]
 * One schedule, sorted by due time, interleaves every slot's shards.
 * A slot is a tag at an address, so a datagram packs at most one
 * fragment a slot and no two fragments of it share a tag.
 */
struct egrEnt {
  unsigned char *work;  /* 0: a free slot */
  struct timespec delay;/* a shard's */
  unsigned int stride;
  unsigned int km;
  unsigned int sent;    /* shards packed so far */
};

struct egrShd {
  struct timespec ts;   /* due */
  unsigned int ent;
  unsigned int shard;
};

/*
 * What the egress holds, for its cleanup handler, run at its exit and
 * at a cancel alike: the thread's own locals, so whatever it holds
 * across a wait goes with it.
 */
struct egrHold {
  struct chanBlbEgrCtx *v;
  struct egrEnt **tab;
  struct egrShd **shd;
  unsigned char **pack;
  unsigned char **seen;
  chanBlb_t **pending;
  unsigned int tableSize;
};

static void
egrFin(
  void *a
){
  struct egrHold *h;
  struct chanBlbChnRsecEgrCtx *ctx;
  void *p;
  unsigned int i;

  h = a;
  ctx = h->v->frmCtx;
  pthread_mutex_lock(&Mu);
  p = ctx->opaque;
  ctx->opaque = 0;
  pthread_mutex_unlock(&Mu);
  h->v->free(p);
  h->v->free(*h->pending);
  if (*h->tab)
    for (i = 0; i < h->tableSize; ++i)
      h->v->free((*h->tab)[i].work);
  h->v->free(*h->shd);
  h->v->free(*h->seen);
  h->v->free(*h->pack);
  h->v->free(*h->tab);
}

void *
chanBlbChnRsecEgr(
  struct chanBlbEgrCtx *v
){
  static unsigned int egrhdr = 5; /* [m(1)][delay_us(4)] after [addrlen][addr][tag] */
  static unsigned long usps = 1000000L;
  static long nsps = 1000000000L;
  const unsigned char *dp[256];
  unsigned char *pp[256];
  struct chanBlbChnRsecEgrCtx *ctx;
  struct chanBlbChnRsecEgrCtrs *priv;
  struct egrHold h;
  struct egrEnt *tab;
  struct egrShd *shd;
  unsigned char *pack;
  unsigned char *seen;
  chanBlb_t *pending;
  chanArr_t p[1];
  struct chanBlbChnRsecEgrCtrs c;
  struct timespec lastSent;
  struct timespec lastPackD;
  struct timespec target;
  unsigned long shdCnt;
  unsigned long shdCap;
  unsigned int maxShard;
  unsigned int tagSize;
  unsigned int tableSize;
  unsigned int ti;
  unsigned char hasSent;
  unsigned char pendingFresh;
  unsigned char shut;

  ctx = v->frmCtx;
  tagSize = ctx->tagSize;
  tableSize = ctx->tableSize;
  tab = 0;
  shd = 0;
  pack = 0;
  seen = 0;
  pending = 0;
  h.v = v;
  h.tab = &tab;
  h.shd = &shd;
  h.pack = &pack;
  h.seen = &seen;
  h.pending = &pending;
  h.tableSize = tableSize;
  pthread_cleanup_push(v->fin, v);
  pthread_cleanup_push(egrFin, &h);
  if (!tableSize
   || (ctx->dtgHashSize && !ctx->dtgHashSign)
   || !(maxShard = chanBlbChnRsecShard(ctx->dgramMax, tagSize, ctx->dtgHashSize))
   || !(priv = v->realloc(0, sizeof (*priv))))
    goto exit;
  memset(priv, 0, sizeof (*priv));
  pthread_mutex_lock(&Mu);
  ctx->opaque = priv;
  pthread_mutex_unlock(&Mu);
  if (!(tab = v->realloc(0, tableSize * sizeof (*tab))))
    goto exit;
  for (ti = 0; ti < tableSize; ++ti)
    tab[ti].work = 0;
  if (!(pack = v->realloc(0, 1 + 255 + ctx->dgramMax))
   || !(seen = v->realloc(0, tableSize)))
    goto exit;
  shdCnt = 0;
  shdCap = 0;
  hasSent = 0;
  pendingFresh = 0;
  shut = 0;
  memset(&c, 0, sizeof (c));
  p[0].c = v->chan;
  p[0].v = (void **)&pending;
  p[0].o = chanOpGet;

  for (;;) {
    struct timespec now;
    long ns;
    int got;

    /* the next datagram's time: the later of the head shard's due time
     * and the last datagram's send plus its packed delays */
    if (!shdCnt)
      ns = 0; /* block forever */
    else {
      target = shd[0].ts;
      if (hasSent) {
        struct timespec t;

        t = lastSent;
        t.tv_sec += lastPackD.tv_sec;
        if ((t.tv_nsec += lastPackD.tv_nsec) >= nsps) {
          ++t.tv_sec;
          t.tv_nsec -= nsps;
        }
        if (TSAFTER(t, target))
          target = t;
      }
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!TSAFTER(target, now))
        ns = -1; /* due now, poll */
      else if (target.tv_sec - now.tv_sec > 1)
        ns = nsps; /* cap at 1s for ILP32 portability */
      else if ((ns = (target.tv_sec - now.tv_sec) * nsps + (target.tv_nsec - now.tv_nsec)) <= 0)
        ns = 1;
    }
    /* once the channel is shut, what it still holds is taken and every
     * message taken goes out at its pace before the framer ends, as a
     * stream's close sends what was written: the Store is read without
     * waiting and the pace kept by sleeping */
    got = 0;
    if (!shut) {
      if (!chanOne(ns, sizeof (p) / sizeof (p[0]), p))
        break;
      if (p[0].s == chanOsGet)
        got = 1;
      else if (p[0].s != chanOsTmo)
        shut = 1;
    } else {
      if (!pending) {
        p[0].v = (void **)&pending;
        p[0].o = chanOpGet;
        if (!(got = chanOne(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet)
         && !shdCnt)
          break;
      }
      if (!got && ns > 0) {
        struct timespec t;

        t.tv_sec = ns / nsps;
        t.tv_nsec = ns % nsps;
        nanosleep(&t, 0);
      }
    }
    if (got) {
      unsigned int prefix;

      /* none (a demand probe), malformed or past chanBlbChnRsecMax at
       * m = 0 (in bytes: the ceiling of k below wraps within maxShard of
       * UINT_MAX): dropped; else held, gets paused and shutdown watched,
       * until it is taken */
      if (pending
       && (pending->l < 1
        || pending->l < (prefix = 1 + pending->b[0] + tagSize) + egrhdr
        || pending->l - (prefix + egrhdr) > 256 * maxShard)) {
        v->free(pending);
        pending = 0;
      }
      if (pending) {
        p[0].v = 0;
        p[0].o = chanOpSht;
        pendingFresh = 1;
      }
    }

    /* At most one datagram a pass, so the pace is read again at each
     * wake.  The pack: the head shard, then every shard due now to the
     * same address from a slot not yet in it, while there is room for
     * the gate.  The wait after it is the SUM of the packed shards'
     * delays. */
    if (shdCnt) {
      struct timespec packD;
      unsigned long si;
      unsigned int frg;
      unsigned int pos;
      unsigned int wp;
      unsigned int ei;

      clock_gettime(CLOCK_MONOTONIC, &now);
      if (TSAFTER(target, now))
        goto take;
      ei = shd[0].ent;
      wp = 1 + tab[ei].work[0];
      memcpy(pack, tab[ei].work, wp);
      memset(seen, 0, tableSize);
      memcpy(pack + wp, tab[ei].work + (unsigned long)shd[0].shard * tab[ei].stride + wp, tab[ei].stride - wp);
      pos = tab[ei].stride;
      seen[ei] = 1;
      packD = tab[ei].delay;
      frg = 1;
      ++tab[ei].sent;
      if (--shdCnt)
        memmove(shd, shd + 1, shdCnt * sizeof (*shd));
      for (si = 0; si < shdCnt && !TSAFTER(shd[si].ts, now);) {
        unsigned int ej;
        unsigned int wj;
        unsigned int fl;

        ej = shd[si].ent;
        wj = 1 + tab[ej].work[0];
        fl = tab[ej].stride - wj;
        if (wj == wp
         && !memcmp(tab[ej].work, pack, wp)
         && !seen[ej]
         && pos + fl + ctx->dtgHashSize <= wp + ctx->dgramMax) {
          memcpy(pack + pos, tab[ej].work + (unsigned long)shd[si].shard * tab[ej].stride + wj, fl);
          pos += fl;
          seen[ej] = 1;
          packD.tv_sec += tab[ej].delay.tv_sec;
          if ((packD.tv_nsec += tab[ej].delay.tv_nsec) >= nsps) {
            ++packD.tv_sec;
            packD.tv_nsec -= nsps;
          }
          ++frg;
          ++tab[ej].sent;
          if (si < --shdCnt)
            memmove(shd + si, shd + si + 1, (shdCnt - si) * sizeof (*shd));
        } else
          ++si;
      }
      if (ctx->dtgHashSize)
        ctx->dtgHashSign(ctx->dtgHashCtx, pack, pack + pos, pack + wp, pos - wp);
      pos += ctx->dtgHashSize;
      if (!v->out(v->outCtx, pack, pos))
        goto exit;
      clock_gettime(CLOCK_MONOTONIC, &lastSent);
      lastPackD = packD;
      hasSent = 1;
      c.frg += frg;
      for (ti = 0; ti < tableSize; ++ti)
        if (seen[ti] && tab[ti].work && tab[ti].sent >= tab[ti].km) {
          ++c.msg;
          v->free(tab[ti].work);
          tab[ti].work = 0;
        }
      pthread_mutex_lock(&Mu);
      *priv = c;
      pthread_mutex_unlock(&Mu);
    }

    /* the blob held: its address and tag still in the table is that
     * message, still going, so it waits for its slot to free, as it
     * does for a free slot on a full table */
take:
    if (pending) {
      struct timespec delay;
      struct timespec ts;
      unsigned char vlq[2];
      unsigned char *work;
      unsigned long delayUs;
      unsigned int prefix;
      unsigned int payLen;
      unsigned int slot;
      unsigned int k;
      unsigned int m;
      unsigned int km;
      unsigned int sz;
      unsigned int pad;
      unsigned int vlqLen;
      unsigned int gap;
      unsigned int stride;
      unsigned int i;

      prefix = 1 + pending->b[0] + tagSize;
      for (slot = tableSize, ti = 0; ti < tableSize; ++ti)
        if (!tab[ti].work) {
          if (slot == tableSize)
            slot = ti;
        } else if (tab[ti].work[0] == pending->b[0]
                && !memcmp(tab[ti].work, pending->b, prefix))
          break;
      if (ti < tableSize || slot == tableSize) {
        if (pendingFresh) {
          pendingFresh = 0;
          if (ti < tableSize)
            ++c.again;
          else
            ++c.full;
          pthread_mutex_lock(&Mu);
          *priv = c;
          pthread_mutex_unlock(&Mu);
        }
        continue;
      }
      pendingFresh = 0;
      m = pending->b[prefix];
      delayUs = (unsigned long)pending->b[prefix + 1] << 24
              | (unsigned long)pending->b[prefix + 2] << 16
              | (unsigned long)pending->b[prefix + 3] << 8
              | pending->b[prefix + 4];
      delay.tv_sec = delayUs / usps;
      delay.tv_nsec = delayUs % usps * 1000;
      payLen = pending->l - (prefix + egrhdr);
      k = payLen ? (payLen + maxShard - 1) / maxShard : 1;
      if ((km = k + m) > 256) {
        m = 256 - k;
        km = 256;
      }
      /* the message's own shard size, tighter than maxShard */
      sz = payLen ? (payLen + k - 1) / k : 0;
      pad = k * sz - payLen;
      if (sz < 128) {
        vlq[0] = sz;
        vlqLen = 1;
      } else {
        vlq[0] = 0x80 | ((sz - 1) >> 7);
        vlq[1] = (sz - 1) & 0x7f;
        vlqLen = 2;
      }
      gap = prefix + 4 + vlqLen;
      stride = gap + sz;
      if (shdCnt + km > shdCap) {
        struct egrShd *t;

        if (!(t = v->realloc(shd, (shdCnt + km) * sizeof (*shd))))
          goto refused;
        shd = t;
        shdCap = shdCnt + km;
      }
      if (!(work = v->realloc(0, (unsigned long)km * stride)))
        goto refused;
      memcpy(work, pending->b, prefix);
      work[prefix] = k - 1;
      work[prefix + 1] = m;
      work[prefix + 2] = 0;
      work[prefix + 3] = pad;
      memcpy(work + prefix + 4, vlq, vlqLen);
      for (i = 0; i < km; ++i) {
        unsigned char *s;

        s = work + (unsigned long)i * stride;
        if (i)
          memcpy(s, work, gap);
        s[prefix + 2] = i;
        if (i < k) {
          unsigned int off;
          unsigned int len;

          off = i * sz;
          if ((len = payLen - off) > sz)
            len = sz;
          if (len)
            memcpy(s + gap, pending->b + prefix + egrhdr + off, len);
          if (len < sz)
            memset(s + gap + len, 0, sz - len);
          dp[i] = s + gap;
        } else
          pp[i - k] = s + gap;
      }
      if (m && sz && rsecEncode(dp, pp, sz, k, m)) {
        v->free(work);
        goto refused;
      }
      tab[slot].work = work;
      tab[slot].delay = delay;
      tab[slot].stride = stride;
      tab[slot].km = km;
      tab[slot].sent = 0;
      /* shard i due at now + (i + 1) x delay, after any already due then */
      clock_gettime(CLOCK_MONOTONIC, &ts);
      for (i = 0; i < km; ++i) {
        unsigned long lo;
        unsigned long hi;

        ts.tv_sec += delay.tv_sec;
        if ((ts.tv_nsec += delay.tv_nsec) >= nsps) {
          ++ts.tv_sec;
          ts.tv_nsec -= nsps;
        }
        lo = 0;
        hi = shdCnt;
        while (lo < hi) {
          unsigned long mid;

          mid = lo + (hi - lo) / 2;
          if (TSAFTER(shd[mid].ts, ts))
            hi = mid;
          else
            lo = mid + 1;
        }
        if (lo < shdCnt)
          memmove(shd + lo + 1, shd + lo, (shdCnt - lo) * sizeof (*shd));
        shd[lo].ts = ts;
        shd[lo].ent = slot;
        shd[lo].shard = i;
        ++shdCnt;
      }
refused:
      v->free(pending);
      pending = 0;
      p[0].v = (void **)&pending;
      p[0].o = chanOpGet;
    }
  }

exit:
  pthread_cleanup_pop(1); /* egrFin(&h) */
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}

/*
 * Ingress.  A reassembly: blob is the message being assembled, data
 * shards in place behind its [addrlen][addr][tag][rm][um] prefix, and 0
 * once it is delivered; the parity shards and the tag ride in mem.
 */
struct igrEnt {
  chanBlb_t *blob;
  unsigned char *parity;   /* m x shardSize, in mem */
  unsigned char *tag;      /* tagSize, in mem after the parity */
  unsigned int k;
  unsigned int m;
  unsigned int shardSize;
  unsigned int padding;
  unsigned int received;   /* distinct fragments of its train */
  unsigned int age;
  unsigned int prefix;     /* 1 + addrlen + tagSize + 2 */
  unsigned char present[256];
  unsigned char mem[1];
};

/* What the ingress holds, for its cleanup handler, as egrHold */
struct igrHold {
  struct chanBlbIgrCtx *v;
  struct igrEnt ***tab;
  unsigned char **buf;
  chanBlb_t **dlv;         /* a blob being put, else 0 */
  unsigned int *used;
};

static void
igrFin(
  void *a
){
  struct igrHold *h;
  struct chanBlbChnRsecIgrCtx *ctx;
  void *p;
  unsigned int i;

  h = a;
  ctx = h->v->frmCtx;
  pthread_mutex_lock(&Mu);
  p = ctx->opaque;
  ctx->opaque = 0;
  pthread_mutex_unlock(&Mu);
  h->v->free(p);
  h->v->free(*h->dlv);
  for (i = 0; i < *h->used; ++i) {
    h->v->free((*h->tab)[i]->blob);
    h->v->free((*h->tab)[i]);
  }
  h->v->free(*h->buf);
  h->v->free(*h->tab);
}

void *
chanBlbChnRsecIgr(
  struct chanBlbIgrCtx *v
){
  unsigned int held[256];  /* tagOwner: the entries by sender */
  struct chanBlbChnRsecIgrCtx *ctx;
  struct chanBlbChnRsecIgrCtrs *priv;
  struct igrHold h;
  struct igrEnt **tab;     /* sorted by tag, used of them */
  struct igrEnt *ent;
  unsigned char *buf;
  chanBlb_t *dlv;
  chanArr_t p[1];
  struct chanBlbChnRsecIgrCtrs c;
  unsigned int tagSize;
  unsigned int tableSize;
  unsigned int tagOwner;
  unsigned int dtgHashSize;
  unsigned int maxShard;   /* a fragment's geometry is egress's for its length */
  unsigned int bufSize;
  unsigned int used;
  unsigned int age;        /* fragments parsed, halved with every entry's */
  unsigned int i;

  ctx = v->frmCtx;
  tagSize = ctx->tagSize;
  tableSize = ctx->tableSize;
  tagOwner = ctx->tagOwner;
  dtgHashSize = ctx->dtgHashSize;
  tab = 0;
  buf = 0;
  dlv = 0;
  used = 0;
  h.v = v;
  h.tab = &tab;
  h.buf = &buf;
  h.dlv = &dlv;
  h.used = &used;
  pthread_cleanup_push(v->fin, v);
  pthread_cleanup_push(igrFin, &h);
  if (!tableSize || tagOwner > tagSize
   || (dtgHashSize && !ctx->dtgHashVrfy)
   || !(maxShard = chanBlbChnRsecShard(ctx->dgramMax, tagSize, dtgHashSize))
   || !(priv = v->realloc(0, sizeof (*priv))))
    goto done;
  memset(priv, 0, sizeof (*priv));
  pthread_mutex_lock(&Mu);
  ctx->opaque = priv;
  pthread_mutex_unlock(&Mu);
  /* the full address range, and one byte past dgramMax, so a datagram
   * the transport cut at the buffer is seen as longer */
  bufSize = 1 + 255 + ctx->dgramMax + 1;
  if (!(tab = v->realloc(0, tableSize * sizeof (*tab)))
   || !(buf = v->realloc(0, bufSize)))
    goto done;
  memset(held, 0, sizeof (held));
  memset(&c, 0, sizeof (c));
  age = 0;
  p[0].c = v->chan;
  p[0].v = (void **)&dlv;
  p[0].o = chanOpPut;

  for (;;) {
    unsigned int n;
    unsigned int wp;
    unsigned int pos;
    unsigned int slot;
    unsigned int lru;
    unsigned int back;     /* after a delivery: 0 the next fragment; an
                            * eviction's, 1 the mismatch, 2 the full table */

    pthread_mutex_lock(&Mu);
    *priv = c;
    pthread_mutex_unlock(&Mu);
    if (!(n = v->blb ? chanBlbIgrBlb(v->free, &v->blb, buf, bufSize)
                     : v->inp(v->inpCtx, buf, bufSize)))
      break;
    wp = 1 + buf[0];
    /* past the wire's size or short of one fragment: not of this wire */
    if (n < wp
     || n - wp > ctx->dgramMax
     || n - wp < tagSize + 5 + dtgHashSize)
      continue;

    /* the GATE, outermost, before any other work */
    if (dtgHashSize) {
      if (!ctx->dtgHashVrfy(ctx->dtgHashCtx, buf, buf + n - dtgHashSize, buf + wp, n - wp - dtgHashSize)) {
        ++c.hash;
        continue;
      }
      n -= dtgHashSize;
    }

    for (pos = wp; pos + tagSize + 5 <= n;) {
      unsigned long ml;
      unsigned int k;
      unsigned int m;
      unsigned int si;
      unsigned int padding;
      unsigned int sz;
      unsigned int vlqLen;
      unsigned int hl;
      unsigned int fragEnd;
      unsigned int vo;
      unsigned int dataPresent;
      int found;

      /* the gate's key is the first fragment's sender: a fragment
       * claiming another is a holder of that key speaking for someone
       * else */
      if (tagOwner && buf[pos + tagOwner - 1] != buf[wp + tagOwner - 1]) {
        ++c.hash;
        break;
      }
      ++c.frg;
      k = buf[pos + tagSize] + 1;
      m = buf[pos + tagSize + 1];
      si = buf[pos + tagSize + 2];
      /* a fragment malformed or cut: the rest of its datagram with it */
      if (k + m > 256 || si >= k + m) {
        ++c.shape;
        break;
      }
      padding = buf[pos + tagSize + 3];
      vo = pos + tagSize + 4;
      if (buf[vo] & 0x80) {
        if (vo + 2 > n) {
          ++c.shape;
          break;
        }
        sz = (((buf[vo] & 0x7f) << 7) | (buf[vo + 1] & 0x7f)) + 1;
        vlqLen = 2;
      } else {
        sz = buf[vo];
        vlqLen = 1;
      }
      hl = tagSize + 4 + vlqLen;
      if ((fragEnd = pos + hl + sz) > n) {
        ++c.shape;
        break;
      }
      /* the geometry egress derives from a message's length and nothing
       * else: what a decode and an entry cost is the message's, never a
       * fragment's to choose.  With the padding under k, the shard is
       * ceil(L / k) */
      ml = (unsigned long)k * sz;
      if (padding >= k || padding > ml
       || ((ml -= padding) ? (ml + maxShard - 1) / maxShard : 1) != k
       || (ctx->msgMax && ml > ctx->msgMax)) {
        ++c.shape;
        pos = fragEnd;
        continue;
      }
      /* halved with every entry's before it can wrap */
      if (++age >= ~0U / 2) {
        for (i = 0; i < used; ++i)
          tab[i]->age >>= 1;
        age >>= 1;
      }
      {
        unsigned int lo;
        unsigned int hi;

        lo = 0;
        hi = used;
        found = 0;
        while (lo < hi) {
          unsigned int mid;
          int r;

          mid = lo + (hi - lo) / 2;
          if ((r = memcmp(buf + pos, tab[mid]->tag, tagSize)) < 0)
            hi = mid;
          else if (r > 0)
            lo = mid + 1;
          else {
            found = 1;
            lo = mid;
            break;
          }
        }
        slot = lo;
      }

      /* a mismatch evicts: padding counts, since it sets the delivered
       * length, and two same-tag payloads fewer than k bytes apart agree
       * on k, m and shard size */
      if (found
       && (tab[slot]->k != k || tab[slot]->m != m
        || tab[slot]->shardSize != sz || tab[slot]->padding != padding)) {
        i = slot;
        found = 0;
        back = 1;
        goto evict;
      }
mismatched:

      /* a train delivered whose k + m fragments have come: the next
       * fragment under its tag can only be a re-send's, a new train */
      if (found && !tab[slot]->blob
       && tab[slot]->received >= tab[slot]->k + tab[slot]->m) {
        if (tagOwner)
          --held[tab[slot]->tag[tagOwner - 1]];
        v->free(tab[slot]);
        if (slot < --used)
          memmove(tab + slot, tab + slot + 1, (used - slot) * sizeof (*tab));
        found = 0;
      }

      /* a new tag on a full table evicts an entry whose train's fragments
       * have all come first, which holds nothing, then the least recently
       * fed, of the sender holding the most where the tag names its
       * sender: one delivered first, which costs nothing, then one in
       * flight, a loss */
      if (!found && used >= tableSize) {
        for (lru = used, i = 0; i < used; ++i)
          if (!tab[i]->blob && tab[i]->received >= tab[i]->k + tab[i]->m
           && (lru == used || tab[i]->age < tab[lru]->age))
            lru = i;
        if (lru == used) {
          unsigned int own;

          own = 0;
          if (tagOwner) {
            own = buf[pos + tagOwner - 1];
            for (i = 0; i < 256; ++i)
              if (held[i] > held[own])
                own = i;
          }
          for (i = 0; i < used; ++i)
            if ((!tagOwner || tab[i]->tag[tagOwner - 1] == own) && !tab[i]->blob
             && (lru == used || tab[i]->age < tab[lru]->age))
              lru = i;
          if (lru == used)
            for (i = 0; i < used; ++i)
              if ((!tagOwner || tab[i]->tag[tagOwner - 1] == own)
               && (lru == used || tab[i]->age < tab[lru]->age))
                lru = i;
        }
        if ((i = lru) < slot)
          --slot;
        back = 2;
        goto evict;
      }
evicted:

      if (!found) {
        if (!(ent = v->realloc(0, sizeof (*ent) + (unsigned long)m * sz + tagSize)))
          break;
        ent->parity = ent->mem;
        ent->tag = ent->mem + (unsigned long)m * sz;
        ent->k = k;
        ent->m = m;
        ent->shardSize = sz;
        ent->padding = padding;
        ent->received = 0;
        ent->age = age;
        ent->prefix = wp + tagSize + 2;
        memcpy(ent->tag, buf + pos, tagSize);
        memset(ent->present, 0, sizeof (ent->present));
        if (!(ent->blob = v->realloc(0, chanBlb_tSize(ent->prefix + k * sz)))) {
          v->free(ent);
          break;
        }
        memcpy(ent->blob->b, buf, wp);
        memcpy(ent->blob->b + wp, buf + pos, tagSize);
        ent->blob->b[wp + tagSize] = 0;
        ent->blob->b[wp + tagSize + 1] = 0;
        if (slot < used)
          memmove(tab + slot + 1, tab + slot, (used - slot) * sizeof (*tab));
        tab[slot] = ent;
        ++used;
        if (tagOwner)
          ++held[ent->tag[tagOwner - 1]];
      }
      ent = tab[slot];

      /* delivered: a straggler, counted against the train, a copy
       * toward nothing */
      if (!ent->blob) {
        ++c.late;
        if (!ent->present[si]) {
          ent->present[si] = 1;
          ++ent->received;
        }
        pos = fragEnd;
        continue;
      }
      if (ent->present[si]) {
        ++c.dup;
        pos = fragEnd;
        continue;
      }
      if (si < k)
        memcpy(ent->blob->b + ent->prefix + (unsigned long)si * sz, buf + pos + hl, sz);
      else
        memcpy(ent->parity + (unsigned long)(si - k) * sz, buf + pos + hl, sz);
      ent->present[si] = 1;
      ++ent->received;
      ent->age = age;
      pos = fragEnd;
      if (ent->received < k)
        continue;

      for (dataPresent = 0, i = 0; i < k; ++i)
        if (ent->present[i])
          ++dataPresent;
      if (dataPresent < k) {
        const unsigned char *rp[256];
        unsigned char *op[256];
        unsigned char idx[256];
        unsigned char *ws;
        unsigned char *cp;
        unsigned int ri;

        if (!(ws = v->realloc(0, RS_WORK_SIZE(k) + (unsigned long)dataPresent * sz)))
          goto drop;
        /* the data shards present are copied out: the output is the blob */
        cp = ws + RS_WORK_SIZE(k);
        for (ri = 0, i = 0; i < k; ++i)
          if (ent->present[i]) {
            memcpy(cp + (unsigned long)ri * sz, ent->blob->b + ent->prefix + (unsigned long)i * sz, sz);
            rp[ri] = cp + (unsigned long)ri * sz;
            idx[ri++] = i;
          }
        for (i = k; i < k + m && ri < k; ++i)
          if (ent->present[i]) {
            rp[ri] = ent->parity + (unsigned long)(i - k) * sz;
            idx[ri++] = i;
          }
        for (i = 0; i < k; ++i)
          op[i] = ent->blob->b + ent->prefix + (unsigned long)i * sz;
        if (rsecDecode(rp, idx, op, sz, k, m, ws)) {
          v->free(ws);
          goto drop;
        }
        v->free(ws);
        ++c.dcd;
      }
      ent->blob->b[ent->prefix - 2] = m;
      ent->blob->b[ent->prefix - 1] = k - dataPresent;
      ent->blob->l = ent->prefix + k * sz - padding;
      ++c.msg;
      dlv = ent->blob;
      ent->blob = 0;
      back = 0;
      goto deliver;

      /* no memory to decode: a loss, as an eviction */
drop:
      i = slot;
      back = 0;

      /* the entry at i evicted, a loss when not yet delivered; slot is
       * the new entry's place once it is gone */
evict:
      ent = tab[i];
      if (i < --used)
        memmove(tab + i, tab + i + 1, (used - i) * sizeof (*tab));
      if (tagOwner)
        --held[ent->tag[tagOwner - 1]];
      dlv = ent->blob;
      if (dlv) {
        ++c.evict;
        if (back == 2)
          ++c.evictFull;
        dlv->b[ent->prefix - 2] = 0;
        dlv->b[ent->prefix - 1] = ent->received;
        dlv->l = ent->prefix;
      }
      v->free(ent);
      if (!dlv)
        goto delivered;

      /* dlv to the channel, then back: an eviction goes on to the
       * fragment that made it, a delivery to the next */
deliver:
      pthread_mutex_lock(&Mu);
      *priv = c;
      pthread_mutex_unlock(&Mu);
      if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1
       || p[0].s != chanOsPut)
        goto done;
      dlv = 0;
delivered:
      if (back == 1)
        goto mismatched;
      if (back == 2)
        goto evicted;
    }
  }

done:
  pthread_cleanup_pop(1); /* igrFin(&h) */
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
