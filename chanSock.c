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

extern void *(*ChanA)(void *, unsigned long);
extern void (*ChanF)(void *);

struct chanSockC {
  chan_t *h; /* no message */
  chan_t *c; /* chanSockM_t message */
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
  chanSockM_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockC, v);
  m = 0;
  pthread_cleanup_push((void(*)(void*))ChanF, m);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet
   && (m->l)
   && write(V->s, m->b, m->l) == m->l) {
    ChanF(m);
    m = 0;
  }
  pthread_cleanup_pop(1); /* ChanF(m) */
  pthread_cleanup_pop(1); /* shutSockC(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* chanShut(V->h) */
  pthread_cleanup_pop(1); /* chanClose(V->h) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

struct chanSockS {
  chan_t *h; /* no message */
  chan_t *c; /* chanSockM_t message */
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
  chanSockM_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockS, v);
  m = 0;
  pthread_cleanup_push((void(*)(void*))ChanF, m);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  while ((m = ChanA(0, sizeof (*m) + V->l - 1))
   && (int)(m->l = read(V->s, m->b, V->l)) > 0) {
    void *t;

    /* attempt to "right size" the message */
    if ((t = ChanA(m, sizeof (*m) + m->l)))
      m = t;
    if (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPut)
      break;
  }
  pthread_cleanup_pop(1); /* ChanF(m) */
  pthread_cleanup_pop(1); /* shutSockS(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* chanShut(V->h) */
  pthread_cleanup_pop(1); /* chanClose(V->h) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

struct chanSockW {
  chan_t *h; /* no message */
  chan_t *r; /* chanSockM_t message */
  chan_t *w; /* chanSockM_t message */
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

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)V->s);
  pthread_cleanup_push((void(*)(void*))chanClose, V->h);
  pthread_cleanup_push((void(*)(void*))chanShut, V->h);
  pthread_cleanup_push((void(*)(void*))chanClose, V->r);
  pthread_cleanup_push((void(*)(void*))chanShut, V->r);
  pthread_cleanup_push((void(*)(void*))chanClose, V->w);
  pthread_cleanup_push((void(*)(void*))chanShut, V->w);
  pthread_cleanup_push((void(*)(void*))shutSockW, v);
  p[0].c = V->h;
  if (!(p[1].c = chanCreate(0,0,0)))
    goto exit0;
  pthread_cleanup_push((void(*)(void*))chanClose, p[1].c);
  if (!(p[2].c = chanCreate(0,0,0)))
    goto exit1;
  pthread_cleanup_push((void(*)(void*))chanClose, p[2].c);
  if (!(xC = ChanA(0, sizeof (*xC))))
    goto exit2;
  chanOpen((xC->h = p[1].c));
  chanOpen((xC->c = V->w));
  xC->s = V->s;
  if (pthread_create(&tC, 0, chanSockC, xC)) {
    chanClose(xC->c);
    chanClose(xC->h);
    ChanF(xC);
    goto exit2;
  }
  pthread_detach(tC);
  if (!(xS = ChanA(0, sizeof (*xS)))) {
    goto exit2;
  }
  chanOpen((xS->h = p[2].c));
  chanOpen((xS->c = V->r));
  xS->s = V->s;
  xS->l = V->l;
  if (pthread_create(&tS, 0, chanSockS, xS)) {
    chanClose(xS->c);
    chanClose(xS->h);
    ChanF(xS);
    goto exit2;
  }
  pthread_detach(tS);
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
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

int
chanSock(
  chan_t *h
 ,chan_t *r
 ,chan_t *w
 ,int s
 ,unsigned int l
){
  struct chanSockW *x;
  pthread_t t;

  x = 0;
  if (!h || !r || !w || s < 0 || !l || !(x = ChanA(0, sizeof (*x))))
    return (0);
  chanOpen((x->h = h));
  chanOpen((x->r = r));
  chanOpen((x->w = w));
  x->s = s;
  x->l = l;
  if (pthread_create(&t, 0, chanSockW, x)) {
    chanClose(x->w);
    chanClose(x->r);
    chanClose(x->h);
    ChanF(x);
    return (0);
  }
  pthread_detach(t);
  return (1);
}
