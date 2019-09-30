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

#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "chan.h"
#include "chanSock.h"

struct chanSockX {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *r;      /* read Chan */
  chan_t *w;      /* write Chan */
  unsigned int l; /* readLimit */
  int s;          /* socket */
};

/* read the chan and write the socket */
static void *
chanSockC(
  void *v
){
  struct chanSockX *x;
  chanSockM_t *m;
  chanPoll_t p[2];

  chanOpen(v);
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  pthread_cleanup_push((void(*)(void*))chanShut, v);
  p[0].c = v;
  p[0].v = (void **)&x;
  p[0].o = chanPoGet;
  p[1].o = chanPoNop;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsGet)
    goto exit0;
  chanOpen(x->w);
  pthread_cleanup_push((void(*)(void*))chanClose, x->w);
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->w;
  p[1].v = (void **)&m;
  p[1].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 2 && p[1].s == chanOsGet
   && (m->l)
   && write(x->s, m->b, m->l) == m->l) {
    x->f(m);
    m = 0;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  shutdown(x->s, SHUT_WR);
  p[0].o = chanPoNop;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 2 && p[1].s == chanOsGet)
    x->f(m);
  pthread_cleanup_pop(1); /* chanClose(x->w) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

/* read the socket and write the chan */
static void *
chanSockS(
  void *v
){
  struct chanSockX *x;
  chanSockM_t *m;
  chanPoll_t p[2];

  chanOpen(v);
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  pthread_cleanup_push((void(*)(void*))chanShut, v);
  p[0].c = v;
  p[0].v = (void **)&x;
  p[0].o = chanPoGet;
  p[1].o = chanPoNop;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsGet)
    goto exit0;
  chanOpen(x->r);
  pthread_cleanup_push((void(*)(void*))chanClose, x->r);
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->r;
  p[1].v = (void **)&m;
  p[1].o = chanPoPut;
  while ((m = x->a(0, sizeof (*m) + x->l - 1))
   && (int)(m->l = read(x->s, m->b, x->l)) > 0) {
    void *t;

    /* attempt to "right size" the message */
    if ((t = x->a(m, sizeof (*m) + m->l)))
      m = t;
    if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 2 || p[1].s != chanOsPut)
      break;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  shutdown(x->s, SHUT_RD);
  pthread_cleanup_pop(1); /* chanClose(x->r) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

/* start reader and writer and wait for chanShuts */
static void *
chanSockW(
  void *v
){
  struct chanSockX *x;
  void *t;
  pthread_t tC;
  pthread_t tS;
  chanPoll_t p[3];

  chanOpen(v);
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  pthread_cleanup_push((void(*)(void*))chanShut, v);
  p[0].c = v;
  p[0].v = (void **)&x;
  p[0].o = chanPoGet;
  p[1].o = chanPoNop;
  p[2].o = chanPoNop;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsGet)
    goto exit0;
  p[0].o = chanPoNop;
  pthread_cleanup_push((void(*)(void*))x->f, x);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)x->s);
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  if (!(p[1].c = chanCreate(x->a, x->f, 0, 0, 0)))
    goto exit1;
  pthread_cleanup_push((void(*)(void*))chanClose, p[1].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[1].c);
  if (!(p[2].c = chanCreate(x->a, x->f, 0, 0, 0)))
    goto exit2;
  pthread_cleanup_push((void(*)(void*))chanClose, p[2].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[2].c);
  if (pthread_create(&tC, 0, chanSockC, p[1].c))
    goto exit3;
  pthread_detach(tC);
  p[1].v = (void **)&x;
  p[1].o = chanPoPutWait;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 2 || p[1].s != chanOsPutWait)
    goto exit3;
  p[1].o = chanPoNop;
  if (pthread_create(&tS, 0, chanSockS, p[2].c))
    goto exit3;
  pthread_detach(tS);
  p[2].v = (void **)&x;
  p[2].o = chanPoPutWait;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 3 || p[2].s != chanOsPutWait)
    goto exit3;
  p[2].o = chanPoNop;
  p[0].v = p[1].v = p[2].v = &t;
  p[0].o = p[1].o = p[2].o = chanPoGet;
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
exit3:
  pthread_cleanup_pop(1); /* chanShut(p[2].c) */
  pthread_cleanup_pop(1); /* chanClose(p[2].c) */
exit2:
  pthread_cleanup_pop(1); /* chanShut(p[1].c) */
  pthread_cleanup_pop(1); /* chanClose(p[1].c) */
exit1:
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  pthread_cleanup_pop(1); /* close(x->s) */
  pthread_cleanup_pop(1); /* x->f(x) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

chan_t *
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,int s
 ,chan_t *r
 ,chan_t *w
 ,unsigned int l
){
  struct chanSockX *x;
  pthread_t t;
  chanPoll_t p[1];

  x = 0;
  if (!a || !f || !l || s < 0 || !r || !w || !(x = a(0, sizeof (*x))))
    return (0);
  p[0].v = (void **)&x;
  if (!(p[0].c = chanCreate(a, f, 0, 0, 0))) {
    f(x);
    return (0);
  }
  x->a = a;
  x->f = f;
  x->s = s;
  x->r = r;
  x->w = w;
  x->l = l;
  if (pthread_create(&t, 0, chanSockW, p[0].c)) {
    chanClose(p[0].c);
    f(x);
    return (0);
  }
  pthread_detach(t);
  p[0].o = chanPoPutWait;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPutWait) {
    chanClose(p[0].c);
    f(x);
    return (0);
  }
  return (p[0].c);
}
