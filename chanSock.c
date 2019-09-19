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
  chan_t *r;
  chan_t *w;
  unsigned int l;
  int d;
};

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
  p[0].o = chanOpPull;
  p[1].o = chanOpNoop;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    goto exit0;
  chanOpen(x->w);
  pthread_cleanup_push((void(*)(void*))chanClose, x->w);
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->w;
  p[1].v = (void **)&m;
  p[1].o = chanOpPull;
  while (chanPoll(0, sizeof(p) / sizeof(p[0]), p) == 2
   && (m->l)
   && write(x->d, m->b, m->l) == m->l) {
    x->f(m);
    m = 0;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  shutdown(x->d, SHUT_WR);
  p[0].o = chanOpNoop;
  while (chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    x->f(m);
  pthread_cleanup_pop(1); /* chanClose(x->w) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return 0;
}

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
  p[0].o = chanOpPull;
  p[1].o = chanOpNoop;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    goto exit0;
  chanOpen(x->r);
  pthread_cleanup_push((void(*)(void*))chanClose, x->r);
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->r;
  p[1].v = (void **)&m;
  p[1].o = chanOpPush;
  while ((m = x->a(0, sizeof(*m) + x->l - 1))
   && (int)(m->l = read(x->d, m->b, x->l)) > 0) {
    void *t;

    if ((t = x->a(m, sizeof(*m) + m->l)))
      m = t;
    if (chanPoll(0, sizeof(p) / sizeof(p[0]), p) != 2)
      break;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  shutdown(x->d, SHUT_RD);
  pthread_cleanup_pop(1); /* chanClose(x->r) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return 0;
}

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
  p[0].o = chanOpPull;
  p[1].o = chanOpNoop;
  p[2].o = chanOpNoop;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    goto exit0;
  p[0].o = chanOpNoop;
  pthread_cleanup_push((void(*)(void*))x->f, x);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)x->d);
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
  p[1].o = chanOpPushWait;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    goto exit3;
  p[1].o = chanOpNoop;
  if (pthread_create(&tS, 0, chanSockS, p[2].c))
    goto exit3;
  pthread_detach(tS);
  p[2].v = (void **)&x;
  p[2].o = chanOpPushWait;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p))
    goto exit3;
  p[2].o = chanOpNoop;
  p[0].v = p[1].v = p[2].v = &t;
  p[0].o = p[1].o = p[2].o = chanOpPull;
  while (p[1].o != chanOpNoop || p[2].o != chanOpNoop) switch (chanPoll(0, sizeof(p) / sizeof(p[0]), p)) {
    case 1:
    case 2:
    case 3:
      /* currently not supporting control messages, waiting for chanShut() */
      break;
    default:
      if (chanIsShut(p[0].c))
        p[1].o = p[2].o = chanOpNoop;
      if (chanIsShut(p[1].c))
        p[1].o = chanOpNoop;
      if (chanIsShut(p[2].c))
        p[2].o = chanOpNoop;
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
  pthread_cleanup_pop(1); /* close(x->d) */
  pthread_cleanup_pop(1); /* x->f(x) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(v) */
  pthread_cleanup_pop(1); /* chanClose(v) */
  return 0;
}

chan_t *
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int l
 ,int d
 ,chan_t *r
 ,chan_t *w
){
  struct chanSockX *x;
  pthread_t t;
  chanPoll_t p[1];

  x = 0;
  if (!a || !f || !l || d < 0 || !r || !w || !(x = a(0, sizeof(*x))))
    return 0;
  p[0].v = (void **)&x;
  if (!(p[0].c = chanCreate(a, f, 0, 0, 0))) {
    f(x);
    return 0;
  }
  x->a = a;
  x->f = f;
  x->l = l;
  x->d = d;
  x->r = r;
  x->w = w;
  if (pthread_create(&t, 0, chanSockW, p[0].c)) {
    chanClose(p[0].c);
    f(x);
    return 0;
  }
  pthread_detach(t);
  p[0].o = chanOpPushWait;
  if (!chanPoll(0, sizeof(p) / sizeof(p[0]), p)) {
    chanClose(p[0].c);
    f(x);
    return 0;
  }
  return p[0].c;
}
