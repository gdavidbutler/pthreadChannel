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
 * chanBlbTrnFdDatagram with combined random delay AND random drop.
 * Delay milliseconds is set via chanBlbTrnFdDatagramDelayMs.
 * Drop percentage is set via chanBlbTrnFdDatagramDropPct (0-100).
 * Each outgoing message is first checked for drop, then if not dropped,
 * enqueued with a random delay in [0, DelayMs] milliseconds.
 *
 * Phase 6: Gilbert-Elliott burst loss (BurstLen > 0) and token bucket
 * bandwidth limiting (BwLimit > 0). Per-destination GE state machines.
 *
 * This file replaces chanBlbTrnFdDatagram.o at link time.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>
#include <sched.h>
#include "chanBlbTrnFdDatagram.h"

/* global delay in milliseconds: 0 = no delay */
unsigned int chanBlbTrnFdDatagramDelayMs = 0;

/* global drop percentage: 0 = no drop, 10 = 10% drop, etc. */
unsigned int chanBlbTrnFdDatagramDropPct = 0;

/* Phase 6: Gilbert-Elliott burst loss */
unsigned int chanBlbTrnFdDatagramBurstLen = 0;  /* mean burst length (0 = independent random) */
unsigned int chanBlbTrnFdDatagramBurstDrop = 80; /* drop% during burst (0-100) */

/* Phase 6: token bucket bandwidth limiting */
unsigned int chanBlbTrnFdDatagramBwLimit = 0;   /* max packets/sec (0 = unlimited) */

struct qMsg {
  struct qMsg *next;
  struct timespec deadline;
  unsigned int len;
  unsigned char data[1]; /* struct hack: actual payload follows */
};

/* Phase 6: per-destination Gilbert-Elliott state */
#define GE_SLOTS 64

struct geSlot {
  unsigned int key; /* hash of destination addr, 0 = empty */
  unsigned char inBad;
};

struct ctx {
  int i4;
  int o4;
  int i6;
  int o6;
  int s; /* bit 2: output active, bit 4: thread running */
  pthread_mutex_t m;
  pthread_cond_t r;
  struct qMsg *qHead;
  struct geSlot ge[GE_SLOTS];
  struct timespec bwLast;
  unsigned int bwTokens;
  unsigned char bwInit;
};
#define V ((struct ctx *)v)

/* dispatch sendto to the correct fd based on sa_family */
static int
sendByFamily(
  struct ctx *c
 ,const unsigned char *b
 ,unsigned int l
){
  unsigned int al;

  if (l < 1)
    return (-1);
  al = b[0];
  if (al > sizeof (struct sockaddr_storage) || l <= 1 + al)
    return (-1);
  switch (((struct sockaddr *)(b + 1))->sa_family) {
  case AF_INET:
    if (c->o4 < 0)
      return (-1);
    return (sendto(c->o4, b + 1 + al, l - 1 - al, 0, (struct sockaddr *)(b + 1), al));
  case AF_INET6:
    if (c->o6 < 0)
      return (-1);
    return (sendto(c->o6, b + 1 + al, l - 1 - al, 0, (struct sockaddr *)(b + 1), al));
  default:
    return (-1);
  }
}

static void *
delayT(
  void *v
){
  struct qMsg *q;

  pthread_mutex_lock(&V->m);
  while ((V->s & 2) || V->qHead) {
    if (!V->qHead) {
      pthread_cond_wait(&V->r, &V->m);
      continue;
    }
    pthread_cond_timedwait(&V->r, &V->m, &V->qHead->deadline);
    while (V->qHead) {
      struct timespec now;

      clock_gettime(CLOCK_REALTIME, &now);
      if (V->qHead->deadline.tv_sec > now.tv_sec
       || (V->qHead->deadline.tv_sec == now.tv_sec
        && V->qHead->deadline.tv_nsec > now.tv_nsec))
        break;
      q = V->qHead;
      V->qHead = q->next;
      pthread_mutex_unlock(&V->m);
      sendByFamily(V, q->data, q->len);
      free(q);
      pthread_mutex_lock(&V->m);
    }
  }
  V->s &= ~4;
  pthread_mutex_unlock(&V->m);
  return (0);
}

void *
chanBlbTrnFdDatagramCtx(
  void *(*ma)(void *, unsigned long)
 ,void (*mf)(void *)
){
  void *v;

  (void)mf;
  mf(ma(0, 1)); /* force exception here and now */
  if ((v = ma(0, sizeof (struct ctx)))) {
    V->i4 = -1;
    V->o4 = -1;
    V->i6 = -1;
    V->o6 = -1;
    V->s = 0;
    V->qHead = 0;
    memset(V->ge, 0, sizeof (V->ge));
    V->bwTokens = 0;
    V->bwInit = 0;
  }
  return (v);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  V->i4 = f4n ? f4[0] : -1;
  V->i6 = f6n ? f6[0] : -1;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  struct pollfd p[2];
  socklen_t sl;
  int i;

  if (l <= 1 + sizeof (struct sockaddr_storage))
    return (0);
  sl = sizeof (struct sockaddr_storage);
  if (V->i6 < 0)
    i = recvfrom(V->i4, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
  else if (V->i4 < 0)
    i = recvfrom(V->i6, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
  else {
    if ((p[0].fd = V->i4) < 0)
      p[0].events = 0;
    else
      p[0].events = POLLIN;
    p[0].revents = 0;
    if ((p[1].fd = V->i6) < 0)
      p[1].events = 0;
    else
      p[1].events = POLLIN;
    p[1].revents = 0;
    if (poll(p, sizeof (p) / sizeof (p[0]), -1) < 0)
      return (0);
    if (p[0].revents == POLLIN)
      i = recvfrom(p[0].fd, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
    else if (p[1].revents == POLLIN)
      i = recvfrom(p[1].fd, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
    else
      i = 0;
  }
  if (i <= 0)
    return (0);
  if ((b[0] = sl) < sizeof (struct sockaddr_storage))
    memmove(b + 1 + sl, b + 1 + sizeof (struct sockaddr_storage), i);
  return (1 + sl + i);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  if (V->i4 >= 0 && V->i4 != V->o4)
    close(V->i4);
  if (V->i6 >= 0 && V->i6 != V->o6)
    close(V->i6);
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  V->o4 = f4n ? f4[0] : -1;
  V->o6 = f6n ? f6[0] : -1;
  if (chanBlbTrnFdDatagramDelayMs > 0) {
    pthread_t th;

    if (pthread_mutex_init(&V->m, 0))
      return (0);
    if (pthread_cond_init(&V->r, 0)) {
      pthread_mutex_destroy(&V->m);
      return (0);
    }
    V->s = 2 | 4;
    if (pthread_create(&th, 0, delayT, v)) {
      V->s = 0;
      pthread_cond_destroy(&V->r);
      pthread_mutex_destroy(&V->m);
      return (0);
    }
    pthread_detach(th);
  }
  return (v);
}

/*
 * Gilbert-Elliott burst drop: two-state Markov chain per destination.
 * Good state drops at DropPct%, Bad state drops at BurstDrop%.
 * State transition probabilities set so that the fraction of time
 * in Bad state equals DropPct/100.
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
  unsigned int dropPct;

  al = b[0];
  key = 5381;
  for (i = 0; i < al; ++i)
    key = ((key << 5) + key) ^ b[1 + i];
  if (key == 0) key = 1;

  /* find or create slot (linear probe) */
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
      g = &c->ge[s];
      break;
    }
  }
  if (!g) {
    g = &c->ge[idx];
    g->key = key;
    g->inBad = 0;
  }

  /* current state determines drop probability.
   * Good-state rate calibrated so overall = DropPct:
   * p_good = DropPct * (100 - BurstDrop) / (100 - DropPct) */
  if (g->inBad) {
    dropPct = chanBlbTrnFdDatagramBurstDrop;
  } else if (chanBlbTrnFdDatagramDropPct > 0
          && chanBlbTrnFdDatagramDropPct < 100) {
    dropPct = chanBlbTrnFdDatagramDropPct
      * (100 - chanBlbTrnFdDatagramBurstDrop)
      / (100 - chanBlbTrnFdDatagramDropPct);
  } else {
    dropPct = chanBlbTrnFdDatagramDropPct;
  }

  /* state transition for next packet */
  if (g->inBad) {
    /* Bad -> Good: probability 1/BurstLen */
    if (arc4random_uniform(chanBlbTrnFdDatagramBurstLen) == 0)
      g->inBad = 0;
  } else if (chanBlbTrnFdDatagramDropPct > 0
          && chanBlbTrnFdDatagramDropPct < 100) {
    /* Good -> Bad: probability 1/G
     * G = BurstLen * (100 - DropPct) / DropPct */
    unsigned int G;

    G = chanBlbTrnFdDatagramBurstLen
      * (100 - chanBlbTrnFdDatagramDropPct)
      / chanBlbTrnFdDatagramDropPct;
    if (G > 0 && arc4random_uniform(G) == 0)
      g->inBad = 1;
  }

  if (dropPct > 0 && arc4random_uniform(100) < dropPct)
    return (1);
  return (0);
}

/*
 * Token bucket bandwidth limiter: cap outgoing packets per second.
 * Tokens accumulate at BwLimit/sec, burst capacity = BwLimit.
 */
static int
bwDrop(
  struct ctx *c
){
  struct timespec now;
  long ms;

  if (chanBlbTrnFdDatagramBwLimit == 0)
    return (0);
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
  if (c->bwTokens > 0) {
    --c->bwTokens;
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
  /* burst drop or independent random drop */
  if (chanBlbTrnFdDatagramBurstLen > 0) {
    if (burstDrop(V, b))
      return (l);
  } else if (chanBlbTrnFdDatagramDropPct > 0
          && arc4random_uniform(100) < chanBlbTrnFdDatagramDropPct) {
    return (l);
  }
  /* bandwidth limit */
  if (bwDrop(V))
    return (l);

  if (chanBlbTrnFdDatagramDelayMs == 0) {
    if (sendByFamily(V, b, l) < 0)
      return (0);
    return (l);
  }
  {
    struct qMsg *q;
    struct qMsg **p;
    struct timespec now;
    unsigned int ms;

    q = (struct qMsg *)malloc(sizeof (*q) - 1 + l);
    if (!q)
      return (0);
    memcpy(q->data, b, l);
    q->len = l;
    ms = arc4random_uniform(chanBlbTrnFdDatagramDelayMs + 1);
    clock_gettime(CLOCK_REALTIME, &now);
    q->deadline.tv_sec = now.tv_sec + ms / 1000;
    q->deadline.tv_nsec = now.tv_nsec + (ms % 1000) * 1000000L;
    if (q->deadline.tv_nsec >= 1000000000L) {
      q->deadline.tv_sec += 1;
      q->deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&V->m);
    for (p = &V->qHead; *p; p = &(*p)->next)
      if (q->deadline.tv_sec < (*p)->deadline.tv_sec
       || (q->deadline.tv_sec == (*p)->deadline.tv_sec
        && q->deadline.tv_nsec < (*p)->deadline.tv_nsec))
        break;
    q->next = *p;
    *p = q;
    pthread_cond_signal(&V->r);
    pthread_mutex_unlock(&V->m);
  }
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  if (chanBlbTrnFdDatagramDelayMs > 0) {
    pthread_mutex_lock(&V->m);
    V->s &= ~2;
    pthread_cond_signal(&V->r);
    pthread_mutex_unlock(&V->m);
  } else {
    if (V->o4 >= 0 && V->o4 != V->i4)
      close(V->o4);
    if (V->o6 >= 0 && V->o6 != V->i6)
      close(V->o6);
  }
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  if (chanBlbTrnFdDatagramDelayMs > 0) {
    pthread_mutex_lock(&V->m);
    while (V->s) {
      pthread_mutex_unlock(&V->m);
      sched_yield();
      pthread_mutex_lock(&V->m);
    }
    pthread_mutex_unlock(&V->m);
    pthread_cond_destroy(&V->r);
    pthread_mutex_destroy(&V->m);
    /* OutputClose deferred close in delay mode; close o now */
    if (V->o4 >= 0 && V->o4 != V->i4)
      close(V->o4);
    if (V->o6 >= 0 && V->o6 != V->i6)
      close(V->o6);
  }
  if (V->i4 >= 0 && V->i4 == V->o4)
    close(V->i4);
  if (V->i6 >= 0 && V->i6 == V->o6)
    close(V->i6);
  free(v);
}

#undef V
