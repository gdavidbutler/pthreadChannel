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
 * chanBlbTrnFdDatagram with queued output delay for latency simulation.
 * Delay milliseconds is set via chanBlbTrnFdDatagramDelayMs.
 * Each message gets a random delay in [0, DelayMs] milliseconds.
 * This file replaces chanBlbTrnFdDatagram.o at link time.
 *
 * Output enqueues messages with a random deadline and returns immediately.
 * A helper thread holds messages until their deadline, then sends them.
 * This simulates random latency without limiting bandwidth.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sched.h>
#include "chanBlbTrnFdDatagram.h"

/* global delay in milliseconds: 0 = no delay */
unsigned int chanBlbTrnFdDatagramDelayMs = 0;

struct qMsg {
  struct qMsg *next;
  struct timespec deadline;
  unsigned int len;
  unsigned char data[1]; /* struct hack: actual payload follows */
};

struct ctx {
  int i;
  int o;
  int s; /* bit 2: output active, bit 4: thread running */
  pthread_mutex_t m;
  pthread_cond_t r;
  struct qMsg *qHead;
};
#define V ((struct ctx *)v)

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
      {
        struct sockaddr_storage a;
        unsigned int al;

        al = q->data[0];
        if (al <= sizeof (a) && q->len >= 1 + al) {
          memcpy(&a, q->data + 1, al);
          sendto(V->o, q->data + 1 + al, q->len - 1 - al, 0, (struct sockaddr *)&a, al);
        }
      }
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
  void
){
  void *v;

  if ((v = malloc(sizeof (struct ctx)))) {
    V->i = -1;
    V->o = -1;
    V->s = 0;
    V->qHead = 0;
  }
  return (v);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,int f
){
  V->i = f;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  struct sockaddr_storage a;
  socklen_t al;
  int i;

  if (l < sizeof (a) + 2)
    return (0);
  al = sizeof (a);
  if ((i = recvfrom(V->i, b + sizeof (a) + 1, l - sizeof (a) - 1, 0, (struct sockaddr *)&a, &al)) <= 0)
    return (0);
  b[0] = (unsigned char)al;
  memcpy(b + 1, &a, al);
  memmove(b + 1 + al, b + sizeof (a) + 1, i);
  return (1 + al + i);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  if (V->i >= 0 && V->i != V->o)
    close(V->i);
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,int f
){
  V->o = f;
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

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  if (chanBlbTrnFdDatagramDelayMs == 0) {
    struct sockaddr_storage a;
    unsigned int al;

    al = b[0];
    if (al > sizeof (a) || l < 1 + al)
      return (0);
    memcpy(&a, b + 1, al);
    if (sendto(V->o, b + 1 + al, l - 1 - al, 0, (struct sockaddr *)&a, al) < 0)
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
    if (V->o >= 0 && V->o != V->i)
      close(V->o);
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
    if (V->o >= 0 && V->o != V->i)
      close(V->o);
  }
  if (V->i == V->o)
    close(V->i);
  free(v);
}

#undef V
