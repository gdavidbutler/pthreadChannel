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

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include "chanBlbTrnKcp.h"
#include "ikcp.h"

static int
output(
  const char *b
 ,int l
 ,ikcpcb *k
 ,void *v
){
  return (write((int)(long)v, b, l));
  (void)k;
}

struct pT {
  void (*f)(void *);
  ikcpcb *v;
  int d;
  int s; /* 0:exit 1:input 2:output 4:thread */
  pthread_mutex_t m;
  pthread_cond_t r;
};

static unsigned int
getMs(
  void
){
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((unsigned int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
}

static void *
pT(
  void *v
){
#define V ((struct pT *)v)
  unsigned int c;
  unsigned int n;
  struct pollfd p;
  int t;
  int i;
  char b[65536];

  p.fd = V->d;
  p.events = POLLIN;
  pthread_mutex_lock(&V->m);
  c = getMs();
  while (V->s & 3) {
    n = ikcp_check(V->v, c);
    pthread_mutex_unlock(&V->m);
    t = (n > c) ? n - c : 10;
    if ((i = poll(&p, 1, t)) < 0) {
      pthread_mutex_lock(&V->m);
      break;
    }
    if (i && (!(p.revents & POLLIN) || (i = read(p.fd, b, sizeof (b))) <= 0)) {
      pthread_mutex_lock(&V->m);
      break;
    }
    pthread_mutex_lock(&V->m);
    if (i) {
      ikcp_input(V->v, b, i);
      if (ikcp_peeksize(V->v)) {
        if (V->s & 1)
          pthread_cond_signal(&V->r);
        else
          ikcp_recv(V->v, b, sizeof (b));
      }
    }
    c = getMs();
    ikcp_update(V->v, c);
  }
  V->s &= ~4;
  pthread_mutex_unlock(&V->m);
  return (0);
#undef V
}

void *
chanBlbTrnKcpCtx(
  void *(*a)(unsigned long)
 ,void (*f)(void *)
 ,int d
 ,unsigned int k
 ,unsigned short iw
 ,unsigned short ow
 ,unsigned short mt
 ,unsigned short nd
 ,unsigned short it
 ,unsigned short rs
 ,unsigned short nc
){
  struct pT *x;
  pthread_t t;

  if (!a || !f)
    return (0);
  f(a(1)); /* force exception here and now */
  if (!(x = a(sizeof (*x)))
   || !(x->v = ikcp_create(k, (void *)(long)d))
   || pthread_mutex_init(&x->m, 0))
    goto error1;
  if (pthread_cond_init(&x->r, 0))
    goto error2;
  x->f = f;
  x->d = d;
  x->s = 7; /* input, output and thread */
  ikcp_setoutput(x->v, output);
  if (ikcp_setmtu(x->v, mt ? mt : 1400)
   || ikcp_wndsize(x->v, ow, iw)
   || ikcp_nodelay(x->v, nd, it, rs, nc))
    goto error3;
  if (pthread_create(&t, 0, pT, x))
    goto error3;
  pthread_detach(t);
  return (x);
error3:
  pthread_cond_destroy(&x->r);
error2:
  pthread_mutex_destroy(&x->m);
error1:
  ikcp_release(x->v);
  f(x);
  return (0);
}

void *
chanBlbTrnKcpInputCtx(
  void *v
){
  return (v);
}

unsigned int
chanBlbTrnKcpInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
#define V ((struct pT *)v)
  unsigned int o;
  int i;

  o = 0;
  pthread_mutex_lock(&V->m);
  if ((V->s & 1)) {
    if ((i = ikcp_recv(V->v, (char *)b, l)) < 0) {
      pthread_cond_wait(&V->r, &V->m);
      if ((i = ikcp_recv(V->v, (char *)b, l)) < 0)
        V->s &= ~1;
      else
        o = i;
    } else
      o = i;
  }
  pthread_mutex_unlock(&V->m);
  return (o);
#undef V
}

void
chanBlbTrnKcpInputClose(
  void *v
){
#define V ((struct pT *)v)
  pthread_mutex_lock(&V->m);
  V->s &= ~1;
  pthread_cond_signal(&V->r);
  pthread_mutex_unlock(&V->m);
#undef V
}

void *
chanBlbTrnKcpOutputCtx(
  void *v
){
  return (v);
}

unsigned int
chanBlbTrnKcpOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
#define V ((struct pT *)v)
  unsigned int o;
  int i;

  o = 0;
  pthread_mutex_lock(&V->m);
  if ((V->s & 2)) {
    if ((i = ikcp_send(V->v, (const char *)b + o, l - o)) < 0)
      V->s &= ~2;
    else
      o = i;
  }
  pthread_mutex_unlock(&V->m);
  return (o);
#undef V
}

void
chanBlbTrnKcpOutputClose(
  void *v
){
#define V ((struct pT *)v)
  pthread_mutex_lock(&V->m);
  V->s &= ~2;
  pthread_mutex_unlock(&V->m);
#undef V
}

void
chanBlbTrnKcpFinalClose(
  void *v
){
#define V ((struct pT *)v)
  pthread_mutex_lock(&V->m);
  while (V->s) {
    pthread_mutex_unlock(&V->m);
    sched_yield();
    pthread_mutex_lock(&V->m);
  }
  close(V->d);
  pthread_cond_destroy(&V->r);
  pthread_mutex_unlock(&V->m);
  pthread_mutex_destroy(&V->m);
  ikcp_release(V->v);
  V->f(v);
#undef V
}
