/*
 * pthreadChannel - an implementation of CSP channels for pthreads
 * Copyright (C) 2019 G. David Butler <gdb@dbSystems.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdlib.h>     /* to support chanSock(0,0 ,...) to indicate realloc() and free() */
#include <unistd.h>     /* for read(), write() and close() */
#include <sys/socket.h> /* for shutdown() */
#include <pthread.h>
#include "chan.h"
#include "chanSock.h"

struct chanSockC {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *h; /* no message */
  chan_t *c; /* chanSock_t message */
  int s;
};

static void
shutSockC(
  void *v
){
#define V ((struct chanSockC *)v)
  shutdown(V->s, SHUT_WR);
#undef V
}

/* get the write chan and write the socket */
static void *
chanSockC(
  void *v
){
#define V ((struct chanSockC *)v)
  chanSock_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockC, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    int f;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    for (l = 0, f = 1; l < m->l && (f = write(V->s, m->b + l, m->l - l)) > 0; l += f);
    pthread_cleanup_pop(1); /* V->f(m) */
    if (f <= 0)
      break;
  }
  pthread_cleanup_pop(1); /* shutSockC(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* chanShut(V->h) */
  pthread_cleanup_pop(1); /* chanClose(V->h) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

struct chanSockS {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *h; /* no message */
  chan_t *c; /* chanSock_t message */
  int s;
  unsigned int l;
};

static void
shutSockS(
  void *v
){
#define V ((struct chanSockS *)v)
  shutdown(V->s, SHUT_RD);
#undef V
}

/* read the socket and put the read chan */
static void *
chanSockS(
  void *v
){
#define V ((struct chanSockS *)v)
  chanSock_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockS, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  while ((m = V->a(0, sizeof (*m) + V->l - sizeof (m->b)))) {
    int f;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    if ((f = read(V->s, m->b, V->l)) > 0) {
      void *t;

      m->l = f;
      /* attempt to "right size" the message */
      if ((t = V->a(m, sizeof (*m) + m->l - sizeof (m->b))))
        m = t;
      f = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    }
    pthread_cleanup_pop(0); /* V->f(m) */
    if (f <= 0) {
      V->f(m);
      break;
    }
  }
  pthread_cleanup_pop(1); /* shutSockS(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* chanShut(V->h) */
  pthread_cleanup_pop(1); /* chanClose(V->h) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

struct chanSockW {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *h; /* no message */
  chan_t *r; /* chanSock_t message */
  chan_t *w; /* chanSock_t message */
  int s;
  unsigned int l;
};

static void
shutSockW(
  void *v
){
#define V ((struct chanSockW *)v)
  shutdown(V->s, SHUT_RDWR);
#undef V
}

/* start channel reader and socket reader and wait for chanShuts */
static void *
chanSockW(
  void *v
){
#define V ((struct chanSockW *)v)
  void *t;
  struct chanSockC *xC;
  struct chanSockS *xS;
  pthread_t tC;
  pthread_t tS;
  chanPoll_t p[3];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)V->s);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->r);
  pthread_cleanup_push((void(*)(void*))chanShut, V->r);
  pthread_cleanup_push((void(*)(void*))chanClose, V->w);
  pthread_cleanup_push((void(*)(void*))chanShut, V->w);
  pthread_cleanup_push((void(*)(void*))shutSockW, v);
  if ((p[0].c = V->h))
    p[0].o = chanPoGet;
  else
    p[0].o = chanPoNop;
  if (V->w) {
    if (!(p[1].c = chanCreate(V->a, V->f, 0,0,0)))
      goto exit0;
    p[1].o = chanPoGet;
  } else {
    p[1].c = 0;
    p[1].o = chanPoNop;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, p[1].c);
  if (V->r) {
    if (!(p[2].c = chanCreate(V->a, V->f, 0,0,0)))
      goto exit1;
    p[2].o = chanPoGet;
  } else {
    p[2].c = 0;
    p[2].o = chanPoNop;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, p[2].c);
  if (V->w) {
    if (!(xC = V->a(0, sizeof (*xC))))
      goto exit2;
    xC->a = V->a;
    xC->f = V->f;
    xC->h = chanOpen(p[1].c);
    xC->c = chanOpen(V->w);
    xC->s = V->s;
    if (pthread_create(&tC, 0, chanSockC, xC)) {
      chanClose(xC->c);
      chanClose(xC->h);
      V->f(xC);
      goto exit2;
    }
    pthread_detach(tC);
  } else
    shutdown(V->s, SHUT_WR);
  if (V->r) {
    if (!(xS = V->a(0, sizeof (*xS)))) {
      goto exit2;
    }
    xS->a = V->a;
    xS->f = V->f;
    xS->h = chanOpen(p[2].c);
    xS->c = chanOpen(V->r);
    xS->s = V->s;
    xS->l = V->l;
    if (pthread_create(&tS, 0, chanSockS, xS)) {
      chanClose(xS->c);
      chanClose(xS->h);
      V->f(xS);
      goto exit2;
    }
    pthread_detach(tS);
  } else
    shutdown(V->s, SHUT_RD);
  p[0].v = p[1].v = p[2].v = &t;
  while (p[1].o != chanPoNop || p[2].o != chanPoNop) switch (chanPoll(-1, sizeof (p) / sizeof (p[0]), p)) {
    case 1:
      if (p[0].s != chanOsGet)
        p[1].o = p[2].o = chanPoNop;
      break;
    case 2:
      if (p[1].s != chanOsGet)
        p[1].o = chanPoNop;
      break;
    case 3:
      if (p[2].s != chanOsGet)
        p[2].o = chanPoNop;
      break;
    default:
      p[1].o = p[2].o = chanPoNop;
      break;
  }
exit2:
  pthread_cleanup_pop(1); /* chanClose(p[2].c) */
exit1:
  pthread_cleanup_pop(1); /* chanClose(p[1].c) */
exit0:
  pthread_cleanup_pop(1); /* shutSockW(v) */
  pthread_cleanup_pop(1); /* chanShut(V->w) */
  pthread_cleanup_pop(1); /* chanClose(V->w) */
  pthread_cleanup_pop(1); /* chanShut(V->r) */
  pthread_cleanup_pop(1); /* chanClose(V->r) */
  pthread_cleanup_pop(1); /* chanShut(V->h) */
  pthread_cleanup_pop(1); /* chanClose(V->h) */
  pthread_cleanup_pop(1); /* close(V->s); */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

int
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *h
 ,chan_t *r
 ,chan_t *w
 ,int s
 ,unsigned int l
){
  struct chanSockW *x;
  pthread_t t;

  if (((a || f) && (!a || !f)) || (!r && !w) || s < 0 || (r && !l))
    return (0);
  if (a) {
    /* force exceptions here and now */
    if (!(x = a(0, sizeof (*x))))
      return (0);
    f(x);
    if (!(x = a(0, sizeof (*x))))
      return (0);
    x->a = a;
    x->f = f;
  } else {
    if (!(x = realloc(0, sizeof (*x))))
      return (0);
    x->a = realloc;
    x->f = free;
  }
  x->h = chanOpen(h);
  x->r = chanOpen(r);
  x->w = chanOpen(w);
  x->s = s;
  x->l = l;
  if (pthread_create(&t, 0, chanSockW, x)) {
    chanClose(x->w);
    chanClose(x->r);
    chanClose(x->h);
    x->f(x);
    return (0);
  }
  pthread_detach(t);
  return (1);
}
