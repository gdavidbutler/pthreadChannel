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
  chanPoll_t h;

  h.c = v;
  h.v = (void **)&x;
  h.o = chanOpRecv;
  if (!chanPoll(0, 1, &h))
    return 0;
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  h.c = x->w;
  h.v = (void **)&m;
  while (chanPoll(0, 1, &h)
   && (m->l)
   && write(x->d, m->b, m->l) == m->l) {
    x->f(m);
    m = 0;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  m = 0;
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  shutdown(x->d, SHUT_WR);
  h.c = v;
  h.o = chanOpSend;
  chanPoll(0, 1, &h);
  h.c = x->w;
  h.o = chanOpRecv;
  while (chanPoll(1, 1, &h))
    x->f(m);
  return 0;
}

static void *
chanSockS(
  void *v
){
  struct chanSockX *x;
  chanSockM_t *m;
  void *t;
  chanPoll_t h;

  h.c = v;
  h.v = (void **)&x;
  h.o = chanOpRecv;
  if (!chanPoll(0, 1, &h))
    return 0;
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  h.c = x->r;
  h.v = (void **)&m;
  h.o = chanOpSend;
  while ((m = x->a(0, sizeof(*m) + x->l - 1))
   && (int)(m->l = read(x->d, m->b, x->l)) > 0) {
    if ((t = x->a(m, sizeof(*m) + m->l)))
      m = t;
    if (!chanPoll(0, 1, &h))
      break;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  m = 0;
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  shutdown(x->d, SHUT_RD);
  h.c = v;
  h.o = chanOpSend;
  chanPoll(0, 1, &h);
  return 0;
}

static void *
chanSockW(
  void *v
){
  struct chanSockX *x;
  void *t;
  pthread_t pC;
  pthread_t pS;
  chanPoll_t h;

  pthread_cleanup_push((void(*)(void*))chanFree, v);
  h.c = v;
  h.v = (void **)&x;
  h.o = chanOpRecv;
  if (!chanPoll(0, 1, &h))
    goto exit0;
  pthread_cleanup_push((void(*)(void*))x->f, x);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)x->d);
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  if (pthread_create(&pC, 0, chanSockC, v))
    goto exit1;
  pthread_cleanup_push((void(*)(void*))pthread_cancel, (void *)pC);
  h.o = chanOpSendWait;
  if (!chanPoll(0, 1, &h))
    goto exit2;
  if (pthread_create(&pS, 0, chanSockS, v))
    goto exit2;
  pthread_cleanup_push((void(*)(void*))pthread_cancel, (void *)pS);
  if (!chanPoll(0, 1, &h))
    goto exit3;
  h.v = &t;
  h.o = chanOpRecv;
  chanPoll(0, 1, &h);
  chanPoll(0, 1, &h);
  pthread_join(pC, 0);
  pthread_join(pS, 0);
exit3:
  shutdown(x->d, SHUT_RDWR);
  pthread_cleanup_pop(1); /* pthread_cancel(pS) */
exit2:
  pthread_cleanup_pop(1); /* pthread_cancel(pC) */
exit1:
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  pthread_cleanup_pop(1); /* close(x->d) */
  pthread_cleanup_pop(1); /* x->f(x) */
exit0:
  pthread_cleanup_pop(1); /* chanFree(v) */
  return 0;
}

pthread_t
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int l
 ,int d
 ,chan_t *r
 ,chan_t *w
){
  struct chanSockX *x;
  pthread_t p;
  chanPoll_t h;

  x = 0;
  if (!a || !f || !l || d < 0 || !r || !w || !(x = a(0, sizeof(*x))))
    return 0;
  if (!(h.c = chanAlloc(a, f, 0, 0, 0))) {
    f(x);
    return 0;
  }
  x->a = a;
  x->f = f;
  x->l = l;
  x->d = d;
  x->r = r;
  x->w = w;
  if (pthread_create(&p, 0, chanSockW, h.c)) {
    chanFree(h.c);
    f(x);
    return 0;
  }
  h.v = (void **)&x;
  h.o = chanOpSend;
  if (!chanPoll(0, 1, &h)) {
    pthread_cancel(p);
    pthread_join(p, 0);
    chanFree(h.c);
    f(x);
    return 0;
  }
  return p;
}
