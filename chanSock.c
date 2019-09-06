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
  chanSockM_t *m;
  void *t;
  struct chanSockX *x;

  if (!chanRecv(0, v, &t))
    return 0;
  x = t;
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  t = 0;
  pthread_cleanup_push((void(*)(void*))x->f, t);
  while (chanRecv(0, x->w, &t)
   && (m = t)
   && (m->l)
   && write(x->d, m->b, m->l) == m->l) {
    x->f(t);
    t = 0;
  }
  pthread_cleanup_pop(1); /* x->f(t) */
  pthread_cleanup_pop(1); /* chanShut(x->w) */
  shutdown(x->d, SHUT_WR);
  chanSend(0, v, 0);
  while (chanRecv(1, x->w, &t))
    x->f(t);
  return 0;
}

static void *
chanSockS(
  void *v
){
  chanSockM_t *m;
  void *t;
  struct chanSockX *x;

  if (!chanRecv(0, v, &t))
    return 0;
  x = t;
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  while ((m = x->a(0, sizeof(*m) + x->l - 1))
   && (int)(m->l = read(x->d, m->b, x->l)) > 0) {
    if ((t = x->a(m, sizeof(*m) + m->l)))
      m = t;
    if (!chanSend(0, x->r, m))
      break;
  }
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->r) */
  shutdown(x->d, SHUT_RD);
  chanSend(0, v, 0);
  return 0;
}

static void *
chanSockW(
  void *v
){
  void *t;
  struct chanSockX *x;
  pthread_t pC;
  pthread_t pS;

  pthread_cleanup_push((void(*)(void*))chanFree, v);
  if (!chanRecv(0, v, &t))
    goto exit0;
  x = t;
  pthread_cleanup_push((void(*)(void*))x->f, x);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)x->d);
  pthread_cleanup_push((void(*)(void*))chanShut, x->r);
  pthread_cleanup_push((void(*)(void*))chanShut, x->w);
  if (pthread_create(&pC, 0, chanSockC, v))
    goto exit1;
  pthread_cleanup_push((void(*)(void*))pthread_cancel, (void *)pC);
  if (!chanSendWait(0, v, x))
    goto exit2;
  if (pthread_create(&pS, 0, chanSockS, v))
    goto exit2;
  pthread_cleanup_push((void(*)(void*))pthread_cancel, (void *)pS);
  if (!chanSendWait(0, v, x))
    goto exit3;
  chanRecv(0, v, &t);
  chanRecv(0, v, &t);
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
  chan_t *c;
  struct chanSockX *x;
  pthread_t p;

  x = 0;
  if (!a || !f || !l || d < 0 || !r || !w || !(x = a(0, sizeof(*x))))
    return 0;
  if (!(c = chanAlloc(a, f, 0, 0, 0))) {
    f(x);
    return 0;
  }
  x->a = a;
  x->f = f;
  x->l = l;
  x->d = d;
  x->r = r;
  x->w = w;
  if (pthread_create(&p, 0, chanSockW, c)) {
    chanFree(c);
    f(x);
    return 0;
  }
  if (!chanSend(0, c, x)) {
    pthread_cancel(p);
    pthread_join(p, 0);
    chanFree(c);
    f(x);
    return 0;
  }
  return p;
}
