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
  m = 0;
  while (chanRecv(0, x->w, &t)
   && (m = t)
   && (m->l)
   && write(x->d, m->b, m->l) == m->l) {
    x->f(m);
    m = 0;
  }
  shutdown(x->d, SHUT_WR);
  chanShut(x->w);
  chanSend(0, v, 0);
  if (m) do {
    x->f(m);
  } while(chanRecv(0, x->w, &t) && (m = t));
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
  while ((m = x->a(0, sizeof(*m) + x->l - 1))
   && (int)(m->l = read(x->d, m->b, x->l)) > 0) {
    if ((t = x->a(m, sizeof(*m) + m->l)))
      m = t;
    if (!chanSend(0, x->r, m))
      break;
  }
  shutdown(x->d, SHUT_RD);
  chanShut(x->r);
  chanSend(0, v, 0);
  if (m) {
    x->f(m);
  }
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

  if (!chanRecv(0, v, &t))
    return 0;
  x = t;
  if (pthread_create(&pC, 0, chanSockC, v))
    goto exit;
  if (!chanSendWait(0, v, x)) {
    pthread_cancel(pC);
    pthread_join(pC, 0);
    goto exit;
  }
  if (pthread_create(&pS, 0, chanSockS, v)) {
    pthread_cancel(pC);
    pthread_join(pC, 0);
    goto exit;
  }
  if (!chanSendWait(0, v, x)) {
    pthread_cancel(pC);
    pthread_cancel(pS);
    pthread_join(pC, 0);
    pthread_join(pS, 0);
    goto exit;
  }
  chanRecv(0, v, &t);
  chanRecv(0, v, &t);
  pthread_join(pC, 0);
  pthread_join(pS, 0);
exit:
  shutdown(x->d, SHUT_RDWR);
  close(x->d);
  chanFree(v);
  x->f(x);
  return 0;
}

int
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
  return 1;
}
