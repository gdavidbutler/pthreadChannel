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

/*
 * In-process loopback link-time replacement for chanBlbTrnFdDatagram.o.
 *
 * Replaces the UDP sendto/recvfrom data path with a process-global address
 * bus.  Ingress InputCtx registrations are keyed by getsockname() of the
 * bound fds; egress Output looks up the destination in the bus and enqueues
 * a [srcAddrLen][srcAddr][payload] record into the matching ingress ctx's
 * in-memory queue.  No kernel UDP, no recvbuf opacity.
 *
 * The fds passed in remain real kernel sockets because callers expect to
 * close() them; we just don't do any I/O on them.  chanBlb's monT wakes a
 * blocked Input via pthread_cancel (Input blocks in pthread_cond_wait,
 * which is a cancellation point).
 *
 * Preserved externals: chanBlbTrnFdDatagramDelayMs, DropPct, BurstLen,
 * BurstDrop, BwLimit, ObsEnable, ObsCnt, Obs[].  Semantics match the prior
 * UDP stress variant: ObsEnable records what the egress *attempted* to
 * emit (pre-drop, pre-delay, pre-bw).  DropPct / burst / bwlimit apply at
 * enqueue time.  DelayMs holds the message in a deadline-sorted queue
 * inside the egress ctx before bus delivery.
 *
 * The egress and ingress halves of a single chanBlb instance may share
 * one ctx (as test_rsec does) or live in two separate ctx objects (as
 * AAB does with its half-duplex instances).  The bus is global so the
 * latter still routes correctly across instance boundaries.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sched.h>
#include "chanBlbTrnFdDatagram.h"

/* ===== Stress knobs (same externs as prior UDP stress variant) ===== */
unsigned int chanBlbTrnFdDatagramDelayMs = 0;
unsigned int chanBlbTrnFdDatagramDropPct = 0;
unsigned int chanBlbTrnFdDatagramBurstLen = 0;
unsigned int chanBlbTrnFdDatagramBurstDrop = 80;
unsigned int chanBlbTrnFdDatagramBwLimit = 0;

#define CHAN_BLB_TRN_FD_DATAGRAM_OBS_MAX 1024
struct chanBlbTrnFdDatagramObsEntry {
  struct timespec ts;
  unsigned int len;
};
unsigned int chanBlbTrnFdDatagramObsEnable = 0;
unsigned int chanBlbTrnFdDatagramObsCnt = 0;
struct chanBlbTrnFdDatagramObsEntry
  chanBlbTrnFdDatagramObs[CHAN_BLB_TRN_FD_DATAGRAM_OBS_MAX];

/* ===== In-memory bus ===== */

/* one ingress-side queued datagram: data[] is [srcAddrLen][srcAddr][payload] */
struct busMsg {
  struct busMsg *next;
  unsigned int len;
  unsigned char data[1];
};

/* one delayed egress datagram: wire[] is [dstAddrLen][dstAddr][payload] */
struct qMsg {
  struct qMsg *next;
  struct timespec deadline;
  struct sockaddr_storage src;
  socklen_t srcLen;
  unsigned int wireLen;
  unsigned char wire[1];
};

#define GE_SLOTS 64
struct geSlot {
  struct timespec lastCheck;
  struct timespec burstEnd;
  unsigned int key;
  unsigned char inBad;
};

struct ctx;

/* one bus registration: links a bound local address to an ingress ctx */
struct busEntry {
  struct busEntry *next;
  struct sockaddr_storage addr;
  socklen_t addrLen;
  struct ctx *target;
};

struct ctx {
  /* egress state (populated by OutputCtx) */
  int o4, o6;
  struct sockaddr_storage src4;
  struct sockaddr_storage src6;
  socklen_t src4Len;
  socklen_t src6Len;
  unsigned char hasSrc4;
  unsigned char hasSrc6;
  unsigned char hasEgr;

  /* delay thread + per-egress GE/bw state */
  pthread_mutex_t delayM;
  pthread_cond_t  delayCv;
  struct qMsg *qHead;
  /* delayS bits: 1=init, 2=output active, 4=thread running */
  int delayS;
  struct geSlot ge[GE_SLOTS];
  struct timespec bwLast;
  unsigned int bwTokens;
  unsigned char bwInit;

  /* ingress state (populated by InputCtx) */
  int i4, i6;
  pthread_mutex_t igrM;
  pthread_cond_t  igrCv;
  struct busMsg *igrHead;
  struct busMsg *igrTail;
  /* array of bus registrations we own (one per bound fd) */
  struct busEntry **busEnts;
  unsigned int busEntsN;
  unsigned char igrInit;
  unsigned char hasIgr;
};
#define V ((struct ctx *)v)

static pthread_mutex_t BusLock = PTHREAD_MUTEX_INITIALIZER;
static struct busEntry *BusHead = 0;

static int
addrEq(
  const struct sockaddr *a
 ,socklen_t al
 ,const struct sockaddr *b
 ,socklen_t bl
){
  if (a->sa_family != b->sa_family)
    return (0);
  if (a->sa_family == AF_INET) {
    const struct sockaddr_in *aa = (const struct sockaddr_in *)a;
    const struct sockaddr_in *bb = (const struct sockaddr_in *)b;
    (void)al; (void)bl;
    return (aa->sin_port == bb->sin_port
         && aa->sin_addr.s_addr == bb->sin_addr.s_addr);
  }
  if (a->sa_family == AF_INET6) {
    const struct sockaddr_in6 *aa = (const struct sockaddr_in6 *)a;
    const struct sockaddr_in6 *bb = (const struct sockaddr_in6 *)b;
    (void)al; (void)bl;
    return (aa->sin6_port == bb->sin6_port
         && memcmp(&aa->sin6_addr, &bb->sin6_addr, sizeof (aa->sin6_addr)) == 0);
  }
  return (0);
}

/*
 * Deliver one datagram to its destination's ingress ctx.
 * wire = [dstAddrLen(1)][dstAddr(dstAddrLen)][payload].
 * src/srcLen identify the sender (copied into the delivered blob as
 * [srcAddrLen][srcAddr]).
 * Returns 1 if delivered, 0 if no bus entry matched (silent drop).
 */
static int
deliverLoopback(
  const unsigned char *wire
 ,unsigned int wireLen
 ,const struct sockaddr *src
 ,socklen_t srcLen
){
  unsigned int dstAddrLen;
  unsigned int payloadLen;
  const struct sockaddr *dst;
  struct busEntry *e;
  struct busMsg *m;
  struct ctx *tgt;
  unsigned int needed;

  if (wireLen < 1)
    return (0);
  dstAddrLen = wire[0];
  if (wireLen < 1 + dstAddrLen)
    return (0);
  dst = (const struct sockaddr *)(wire + 1);
  payloadLen = wireLen - 1 - dstAddrLen;

  if (srcLen > sizeof (struct sockaddr_storage))
    srcLen = sizeof (struct sockaddr_storage);

  needed = 1 + (unsigned int)srcLen + payloadLen;

  pthread_mutex_lock(&BusLock);
  for (e = BusHead; e; e = e->next) {
    if (addrEq(dst, (socklen_t)dstAddrLen,
               (const struct sockaddr *)&e->addr, e->addrLen))
      break;
  }
  if (!e) {
    pthread_mutex_unlock(&BusLock);
    return (0);
  }
  tgt = e->target;

  m = (struct busMsg *)malloc(sizeof (struct busMsg) - 1 + needed);
  if (!m) {
    pthread_mutex_unlock(&BusLock);
    return (0);
  }
  m->next = 0;
  m->len = needed;
  m->data[0] = (unsigned char)srcLen;
  memcpy(m->data + 1, src, srcLen);
  memcpy(m->data + 1 + srcLen, wire + 1 + dstAddrLen, payloadLen);

  pthread_mutex_lock(&tgt->igrM);
  if (tgt->igrTail) {
    tgt->igrTail->next = m;
    tgt->igrTail = m;
  } else {
    tgt->igrHead = m;
    tgt->igrTail = m;
  }
  pthread_cond_signal(&tgt->igrCv);
  pthread_mutex_unlock(&tgt->igrM);
  pthread_mutex_unlock(&BusLock);
  return (1);
}

/*
 * Test-only: inject a payload directly into whichever ingress ctx is
 * registered at dst, with the given synthetic src prefix.  Bypasses
 * every egress filter (no drop, no delay, no bandwidth accounting, no
 * observation record).  Returns 1 on delivery, 0 on no-route-to-dst
 * or allocation failure.  Used by test_rsec.c to replicate the old
 * "sendto from a second UDP socket" pattern now that the data path
 * is fully in-process.
 */
int
chanBlbTrnFdDatagramInject(
  const struct sockaddr *dst
 ,socklen_t dstLen
 ,const struct sockaddr *src
 ,socklen_t srcLen
 ,const unsigned char *payload
 ,unsigned int payloadLen
){
  unsigned char *wire;
  unsigned int wireLen;
  int rv;

  if (dstLen < 1 || dstLen > 255)
    return (0);
  wireLen = 1 + (unsigned int)dstLen + payloadLen;
  wire = (unsigned char *)malloc(wireLen);
  if (!wire)
    return (0);
  wire[0] = (unsigned char)dstLen;
  memcpy(wire + 1, dst, dstLen);
  if (payloadLen > 0)
    memcpy(wire + 1 + dstLen, payload, payloadLen);
  rv = deliverLoopback(wire, wireLen, src, srcLen);
  free(wire);
  return (rv);
}

static const struct sockaddr *
pickSrc(
  struct ctx *c
 ,int family
 ,socklen_t *outLen
){
  if (family == AF_INET && c->hasSrc4) {
    *outLen = c->src4Len;
    return ((const struct sockaddr *)&c->src4);
  }
  if (family == AF_INET6 && c->hasSrc6) {
    *outLen = c->src6Len;
    return ((const struct sockaddr *)&c->src6);
  }
  *outLen = 0;
  return (0);
}

/* apply src addr to wire + deliver; returns 0 on unroutable drop, wireLen otherwise */
static unsigned int
egrSend(
  struct ctx *c
 ,const unsigned char *wire
 ,unsigned int wireLen
){
  const struct sockaddr *src;
  socklen_t srcLen;
  unsigned int dstAddrLen;
  int family;

  if (wireLen < 1)
    return (0);
  dstAddrLen = wire[0];
  if (wireLen < 1 + dstAddrLen)
    return (0);
  family = ((const struct sockaddr *)(wire + 1))->sa_family;
  src = pickSrc(c, family, &srcLen);
  if (!src) {
    /* no source for this family configured — silent drop */
    return (wireLen);
  }
  deliverLoopback(wire, wireLen, src, srcLen);
  /* deliver success/fail both return "sent" — UDP semantics */
  return (wireLen);
}

/* ===== delay thread ===== */
static void *
delayT(
  void *v
){
  struct qMsg *q;

  pthread_mutex_lock(&V->delayM);
  while ((V->delayS & 2) || V->qHead) {
    if (!V->qHead) {
      pthread_cond_wait(&V->delayCv, &V->delayM);
      continue;
    }
    pthread_cond_timedwait(&V->delayCv, &V->delayM, &V->qHead->deadline);
    while (V->qHead) {
      struct timespec now;

      clock_gettime(CLOCK_REALTIME, &now);
      if (V->qHead->deadline.tv_sec > now.tv_sec
       || (V->qHead->deadline.tv_sec == now.tv_sec
        && V->qHead->deadline.tv_nsec > now.tv_nsec))
        break;
      q = V->qHead;
      V->qHead = q->next;
      pthread_mutex_unlock(&V->delayM);
      deliverLoopback(q->wire, q->wireLen,
                      (const struct sockaddr *)&q->src, q->srcLen);
      free(q);
      pthread_mutex_lock(&V->delayM);
    }
  }
  V->delayS &= ~4;
  pthread_cond_broadcast(&V->delayCv);
  pthread_mutex_unlock(&V->delayM);
  return (0);
}

/* ===== Ctx allocator ===== */
void *
chanBlbTrnFdDatagramCtx(
  void *(*ma)(void *, unsigned long)
 ,void (*mf)(void *)
){
  void *v;

  mf(ma(0, 1)); /* force exception here and now */
  if (!(v = ma(0, sizeof (struct ctx))))
    return (0);
  memset(v, 0, sizeof (struct ctx));
  V->o4 = -1;
  V->o6 = -1;
  V->i4 = -1;
  V->i6 = -1;
  return (v);
}

/* ===== Ingress ===== */

static int
busRegister(
  struct ctx *c
 ,int fd
){
  struct sockaddr_storage ss;
  socklen_t sl;
  struct busEntry *e;
  struct busEntry **nbuf;

  sl = sizeof (ss);
  if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
    return (-1);
  e = (struct busEntry *)malloc(sizeof (struct busEntry));
  if (!e)
    return (-1);
  memcpy(&e->addr, &ss, sl);
  e->addrLen = sl;
  e->target = c;

  nbuf = (struct busEntry **)realloc(c->busEnts,
    (unsigned long)(c->busEntsN + 1) * sizeof (struct busEntry *));
  if (!nbuf) {
    free(e);
    return (-1);
  }
  c->busEnts = nbuf;
  c->busEnts[c->busEntsN++] = e;

  pthread_mutex_lock(&BusLock);
  e->next = BusHead;
  BusHead = e;
  pthread_mutex_unlock(&BusLock);
  return (0);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  unsigned int j;

  if (!V->igrInit) {
    if (pthread_mutex_init(&V->igrM, 0))
      return (0);
    if (pthread_cond_init(&V->igrCv, 0)) {
      pthread_mutex_destroy(&V->igrM);
      return (0);
    }
    V->igrInit = 1;
  }
  V->hasIgr = 1;

  /* remember the primary v4/v6 fds so Close can close them */
  if (f4n > 0)
    V->i4 = f4[0];
  if (f6n > 0)
    V->i6 = f6[0];

  for (j = 0; j < f4n; ++j)
    (void)busRegister(V, f4[j]);
  for (j = 0; j < f6n; ++j)
    (void)busRegister(V, f6[j]);
  return (v);
}

static void
igrUnlockHelper(
  void *arg
){
  pthread_mutex_unlock((pthread_mutex_t *)arg);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  struct busMsg *m;
  unsigned int rv;

  if (!V->igrInit)
    return (0);

  pthread_mutex_lock(&V->igrM);
  pthread_cleanup_push(igrUnlockHelper, &V->igrM);
  while (!V->igrHead)
    pthread_cond_wait(&V->igrCv, &V->igrM);
  m = V->igrHead;
  V->igrHead = m->next;
  if (!V->igrHead)
    V->igrTail = 0;
  pthread_cleanup_pop(1); /* unlock V->igrM */

  rv = m->len;
  if (rv > l)
    rv = l;
  memcpy(b, m->data, rv);
  free(m);
  return (rv);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  unsigned int j;
  struct busMsg *m;
  struct busMsg *mNext;

  if (!V->hasIgr)
    return;

  /* unlink all our bus entries, then free them outside the global lock */
  pthread_mutex_lock(&BusLock);
  for (j = 0; j < V->busEntsN; ++j) {
    struct busEntry *e = V->busEnts[j];
    struct busEntry **pp;

    for (pp = &BusHead; *pp; pp = &(*pp)->next) {
      if (*pp == e) {
        *pp = e->next;
        break;
      }
    }
  }
  pthread_mutex_unlock(&BusLock);

  for (j = 0; j < V->busEntsN; ++j)
    free(V->busEnts[j]);
  free(V->busEnts);
  V->busEnts = 0;
  V->busEntsN = 0;

  /* drain any pending messages that snuck in after unregister */
  pthread_mutex_lock(&V->igrM);
  m = V->igrHead;
  V->igrHead = 0;
  V->igrTail = 0;
  pthread_mutex_unlock(&V->igrM);
  while (m) {
    mNext = m->next;
    free(m);
    m = mNext;
  }

  if (V->i4 >= 0 && V->i4 != V->o4) {
    close(V->i4);
    V->i4 = -1;
  }
  if (V->i6 >= 0 && V->i6 != V->o6) {
    close(V->i6);
    V->i6 = -1;
  }
}

/* ===== Egress ===== */

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  V->hasEgr = 1;
  V->o4 = f4n ? f4[0] : -1;
  V->o6 = f6n ? f6[0] : -1;

  /* capture per-family source addrs for delivered wire prefix.
   * Unbound sockets yield wildcard addrs; callers using tag-based
   * identity (AAB) don't care.  Callers that do (test_rsec) bind
   * before handing the fd in, so getsockname returns the real addr. */
  if (V->o4 >= 0) {
    socklen_t sl = sizeof (V->src4);

    if (getsockname(V->o4, (struct sockaddr *)&V->src4, &sl) == 0) {
      V->src4Len = sl;
      V->hasSrc4 = 1;
    }
  }
  if (V->o6 >= 0) {
    socklen_t sl = sizeof (V->src6);

    if (getsockname(V->o6, (struct sockaddr *)&V->src6, &sl) == 0) {
      V->src6Len = sl;
      V->hasSrc6 = 1;
    }
  }

  if (chanBlbTrnFdDatagramDelayMs > 0) {
    pthread_t th;

    if (pthread_mutex_init(&V->delayM, 0))
      return (0);
    if (pthread_cond_init(&V->delayCv, 0)) {
      pthread_mutex_destroy(&V->delayM);
      return (0);
    }
    V->delayS = 1 | 2 | 4;
    if (pthread_create(&th, 0, delayT, v)) {
      V->delayS = 0;
      pthread_cond_destroy(&V->delayCv);
      pthread_mutex_destroy(&V->delayM);
      return (0);
    }
    pthread_detach(th);
  }
  return (v);
}

/*
 * Time-based Gilbert-Elliott burst drop per destination.  Unchanged from
 * the prior UDP stress variant.
 */
static int
burstDrop(
  struct ctx *c
 ,const unsigned char *b
){
  unsigned int al;
  unsigned int key;
  unsigned int idx;
  unsigned int i;
  struct geSlot *g;
  struct timespec now;

  al = b[0];
  key = 5381;
  for (i = 0; i < al; ++i)
    key = ((key << 5) + key) ^ b[1 + i];
  if (key == 0) key = 1;

  clock_gettime(CLOCK_MONOTONIC, &now);

  idx = key % GE_SLOTS;
  g = 0;
  for (i = 0; i < GE_SLOTS; ++i) {
    unsigned int s;

    s = (idx + i) % GE_SLOTS;
    if (c->ge[s].key == key) {
      g = &c->ge[s];
      break;
    }
    if (c->ge[s].key == 0) {
      c->ge[s].key = key;
      c->ge[s].inBad = 0;
      c->ge[s].lastCheck = now;
      g = &c->ge[s];
      break;
    }
  }
  if (!g) {
    g = &c->ge[idx];
    g->key = key;
    g->inBad = 0;
    g->lastCheck = now;
  }

  if (g->inBad
   && (now.tv_sec > g->burstEnd.tv_sec
    || (now.tv_sec == g->burstEnd.tv_sec
     && now.tv_nsec >= g->burstEnd.tv_nsec))) {
    g->inBad = 0;
    g->lastCheck = now;
  }

  if (!g->inBad
   && chanBlbTrnFdDatagramDropPct > 0
   && chanBlbTrnFdDatagramDropPct < chanBlbTrnFdDatagramBurstDrop) {
    long dsec;
    long dns;
    unsigned long elapsed_ms;
    unsigned long mean_good_ms;

    dsec = (long)(now.tv_sec - g->lastCheck.tv_sec);
    dns = now.tv_nsec - g->lastCheck.tv_nsec;
    if (dns < 0) {
      dsec--;
      dns += 1000000000L;
    }
    elapsed_ms = dsec > 0
      ? (unsigned long)dsec * 1000 + (unsigned long)dns / 1000000
      : (unsigned long)dns / 1000000;
    g->lastCheck = now;

    mean_good_ms = (unsigned long)chanBlbTrnFdDatagramBurstLen
      * (chanBlbTrnFdDatagramBurstDrop - chanBlbTrnFdDatagramDropPct)
      / chanBlbTrnFdDatagramDropPct;
    if (elapsed_ms > mean_good_ms)
      elapsed_ms = mean_good_ms;
    if (mean_good_ms > 0 && elapsed_ms > 0
     && arc4random_uniform(
          mean_good_ms > 0xfffffffeUL
            ? 0xfffffffeU : (unsigned int)mean_good_ms)
        < (elapsed_ms > 0xfffffffeUL
            ? 0xfffffffeU : (unsigned int)elapsed_ms)) {
      unsigned int burst_ms;

      g->inBad = 1;
      burst_ms = 1 + arc4random_uniform(
        2 * chanBlbTrnFdDatagramBurstLen);
      g->burstEnd.tv_sec = now.tv_sec + burst_ms / 1000;
      g->burstEnd.tv_nsec = now.tv_nsec
        + (long)(burst_ms % 1000) * 1000000L;
      if (g->burstEnd.tv_nsec >= 1000000000L) {
        g->burstEnd.tv_sec++;
        g->burstEnd.tv_nsec -= 1000000000L;
      }
    }
  }

  if (g->inBad
   && chanBlbTrnFdDatagramBurstDrop > 0
   && arc4random_uniform(100) < chanBlbTrnFdDatagramBurstDrop)
    return (1);
  return (0);
}

static int
bwDrop(
  struct ctx *c
 ,unsigned int len
){
  struct timespec now;
  long ms;
  unsigned int cost;

  if (chanBlbTrnFdDatagramBwLimit == 0)
    return (0);
  cost = len * 8;
  clock_gettime(CLOCK_REALTIME, &now);
  if (!c->bwInit) {
    c->bwLast = now;
    c->bwTokens = chanBlbTrnFdDatagramBwLimit;
    c->bwInit = 1;
  }
  ms = (long)(now.tv_sec - c->bwLast.tv_sec) * 1000
    + (now.tv_nsec - c->bwLast.tv_nsec) / 1000000;
  if (ms > 0) {
    unsigned int refill;

    refill = (unsigned int)((unsigned long)chanBlbTrnFdDatagramBwLimit
      * (unsigned long)ms / 1000);
    if (refill > 0) {
      c->bwTokens += refill;
      if (c->bwTokens > chanBlbTrnFdDatagramBwLimit)
        c->bwTokens = chanBlbTrnFdDatagramBwLimit;
      c->bwLast = now;
    }
  }
  if (c->bwTokens >= cost) {
    c->bwTokens -= cost;
    return (0);
  }
  return (1);
}

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  if (chanBlbTrnFdDatagramObsEnable
   && chanBlbTrnFdDatagramObsCnt < CHAN_BLB_TRN_FD_DATAGRAM_OBS_MAX
   && l >= 1
   && l > 1 + (unsigned int)b[0]) {
    struct chanBlbTrnFdDatagramObsEntry *e;

    e = &chanBlbTrnFdDatagramObs[chanBlbTrnFdDatagramObsCnt];
    clock_gettime(CLOCK_MONOTONIC, &e->ts);
    e->len = l - 1 - (unsigned int)b[0];
    ++chanBlbTrnFdDatagramObsCnt;
  }

  if (chanBlbTrnFdDatagramBurstLen > 0) {
    if (burstDrop(V, b))
      return (l);
  } else if (chanBlbTrnFdDatagramDropPct > 0
          && arc4random_uniform(100) < chanBlbTrnFdDatagramDropPct) {
    return (l);
  }
  if (bwDrop(V, l))
    return (l);

  if (chanBlbTrnFdDatagramDelayMs == 0)
    return (egrSend(V, b, l));

  {
    struct qMsg *q;
    struct qMsg **p;
    struct timespec now;
    const struct sockaddr *src;
    socklen_t srcLen;
    unsigned int dstAddrLen;
    int family;
    unsigned int ms;

    if (l < 1) return (0);
    dstAddrLen = b[0];
    if (l < 1 + dstAddrLen) return (0);
    family = ((const struct sockaddr *)(b + 1))->sa_family;
    src = pickSrc(V, family, &srcLen);
    if (!src)
      return (l); /* no source for family — drop like egrSend */

    q = (struct qMsg *)malloc(sizeof (*q) - 1 + l);
    if (!q)
      return (0);
    memcpy(q->wire, b, l);
    q->wireLen = l;
    memcpy(&q->src, src, srcLen);
    q->srcLen = srcLen;
    ms = arc4random_uniform(chanBlbTrnFdDatagramDelayMs + 1);
    clock_gettime(CLOCK_REALTIME, &now);
    q->deadline.tv_sec = now.tv_sec + ms / 1000;
    q->deadline.tv_nsec = now.tv_nsec + (ms % 1000) * 1000000L;
    if (q->deadline.tv_nsec >= 1000000000L) {
      q->deadline.tv_sec += 1;
      q->deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&V->delayM);
    for (p = &V->qHead; *p; p = &(*p)->next)
      if (q->deadline.tv_sec < (*p)->deadline.tv_sec
       || (q->deadline.tv_sec == (*p)->deadline.tv_sec
        && q->deadline.tv_nsec < (*p)->deadline.tv_nsec))
        break;
    q->next = *p;
    *p = q;
    pthread_cond_signal(&V->delayCv);
    pthread_mutex_unlock(&V->delayM);
  }
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  if (V->delayS & 1) {
    pthread_mutex_lock(&V->delayM);
    V->delayS &= ~2;
    pthread_cond_signal(&V->delayCv);
    pthread_mutex_unlock(&V->delayM);
  } else {
    if (V->o4 >= 0 && V->o4 != V->i4) {
      close(V->o4);
      V->o4 = -1;
    }
    if (V->o6 >= 0 && V->o6 != V->i6) {
      close(V->o6);
      V->o6 = -1;
    }
  }
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  if (V->delayS & 1) {
    pthread_mutex_lock(&V->delayM);
    while (V->delayS & 4) {
      pthread_mutex_unlock(&V->delayM);
      sched_yield();
      pthread_mutex_lock(&V->delayM);
    }
    pthread_mutex_unlock(&V->delayM);
    pthread_cond_destroy(&V->delayCv);
    pthread_mutex_destroy(&V->delayM);
    if (V->o4 >= 0 && V->o4 != V->i4) {
      close(V->o4);
      V->o4 = -1;
    }
    if (V->o6 >= 0 && V->o6 != V->i6) {
      close(V->o6);
      V->o6 = -1;
    }
  }
  if (V->i4 >= 0 && V->i4 == V->o4) {
    close(V->i4);
    V->i4 = -1;
  }
  if (V->i6 >= 0 && V->i6 == V->o6) {
    close(V->i6);
    V->i6 = -1;
  }
  if (V->igrInit) {
    pthread_cond_destroy(&V->igrCv);
    pthread_mutex_destroy(&V->igrM);
  }
  free(v);
}

#undef V
