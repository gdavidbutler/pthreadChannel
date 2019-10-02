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
  chan_t *hup;
  chan_t *get;
  chan_t *put;
  unsigned int rl;
  int sk;
};

/* read the chan and write the socket */
static void *
chanSockC(
  void *v
){
  struct chanSockX *x;
  chanSockM_t *m;
  chanPoll_t p[2];

  p[0].o = p[1].o = chanPoNop;
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  pthread_cleanup_push((void(*)(void*))chanShut, v);
  p[0].c = v;
  p[0].v = (void **)&x;
  p[0].o = chanPoGet;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsGet)
    goto exit0;
  pthread_cleanup_push((void(*)(void*))chanShut, x->get);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->get;
  p[1].v = (void **)&m;
  p[1].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 2 && p[1].s == chanOsGet
   && (m->l)
   && write(x->sk, m->b, m->l) == m->l) {
    x->f(m);
    m = 0;
  }
  shutdown(x->sk, SHUT_WR);
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->get) */
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

  p[0].o = p[1].o = chanPoNop;
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  pthread_cleanup_push((void(*)(void*))chanShut, v);
  p[0].c = v;
  p[0].v = (void **)&x;
  p[0].o = chanPoGet;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsGet)
    goto exit0;
  pthread_cleanup_push((void(*)(void*))chanShut, x->put);
  p[0].v = (void **)&m;
  m = 0;
  pthread_cleanup_push((void(*)(void*))x->f, m);
  p[1].c = x->put;
  p[1].v = (void **)&m;
  p[1].o = chanPoPut;
  while ((m = x->a(0, sizeof (*m) + x->rl - 1))
   && (int)(m->l = read(x->sk, m->b, x->rl)) > 0) {
    void *t;

    /* attempt to "right size" the message */
    if ((t = x->a(m, sizeof (*m) + m->l)))
      m = t;
    if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 2 || p[1].s != chanOsPut)
      break;
  }
  shutdown(x->sk, SHUT_RD);
  pthread_cleanup_pop(1); /* x->f(m) */
  pthread_cleanup_pop(1); /* chanShut(x->put) */
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
#define X ((struct chanSockX *)v)
  void *t;
  pthread_t tC;
  pthread_t tS;
  chanPoll_t p[3];

  p[0].o = p[1].o = p[2].o = chanPoNop;
  pthread_cleanup_push((void(*)(void*))X->f, v);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)X->sk);
  pthread_cleanup_push((void(*)(void*))chanClose, X->hup);
  pthread_cleanup_push((void(*)(void*))chanShut, X->hup);
  p[0].c = X->hup;
  pthread_cleanup_push((void(*)(void*))chanShut, X->put);
  pthread_cleanup_push((void(*)(void*))chanShut, X->get);
  if (!(p[1].c = chanCreate(X->a, X->f, 0, 0, 0)))
    goto exit0;
  pthread_cleanup_push((void(*)(void*))chanClose, p[1].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[1].c);
  if (!(p[2].c = chanCreate(X->a, X->f, 0, 0, 0)))
    goto exit1;
  pthread_cleanup_push((void(*)(void*))chanClose, p[2].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[2].c);
  chanOpen(p[1].c);
  if (pthread_create(&tC, 0, chanSockC, p[1].c)) {
    chanClose(p[1].c);
    goto exit2;
  }
  pthread_detach(tC);
  p[1].v = &v;
  p[1].o = chanPoPutWait;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 2 || p[1].s != chanOsPutWait)
    goto exit2;
  p[1].o = chanPoNop;
  chanOpen(p[2].c);
  if (pthread_create(&tS, 0, chanSockS, p[2].c)) {
    chanClose(p[2].c);
    goto exit2;
  }
  pthread_detach(tS);
  p[2].v = &v;
  p[2].o = chanPoPutWait;
  if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 3 || p[2].s != chanOsPutWait)
    goto exit2;
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
  shutdown(X->sk, SHUT_RDWR);
exit2:
  pthread_cleanup_pop(1); /* chanShut(p[2].c) */
  pthread_cleanup_pop(1); /* chanClose(p[2].c) */
exit1:
  pthread_cleanup_pop(1); /* chanShut(p[1].c) */
  pthread_cleanup_pop(1); /* chanClose(p[1].c) */
exit0:
  pthread_cleanup_pop(1); /* chanShut(X->get) */
  pthread_cleanup_pop(1); /* chanShut(X->put) */
  pthread_cleanup_pop(1); /* chanShut(X->hup) */
  pthread_cleanup_pop(1); /* chanClose(X->hup) */
  pthread_cleanup_pop(1); /* close(X->sk); */
  pthread_cleanup_pop(1); /* X->f(v) */
  return (0);
#undef X
}

int
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *hup
 ,int sk
 ,chan_t *get
 ,chan_t *put
 ,unsigned int rl
){
  struct chanSockX *x;
  pthread_t t;

  x = 0;
  if (!a || !f || !hup || sk < 0 || !get || !put || !rl || !(x = a(0, sizeof (*x))))
    return (0);
  x->a = a;
  x->f = f;
  chanOpen((x->hup = hup));
  x->sk = sk;
  chanOpen((x->get = get));
  chanOpen((x->put = put));
  x->rl = rl;
  if (pthread_create(&t, 0, chanSockW, x)) {
    chanClose(x->put);
    chanClose(x->get);
    chanClose(x->hup);
    f(x);
    return (0);
  }
  pthread_detach(t);
  return (1);
}
