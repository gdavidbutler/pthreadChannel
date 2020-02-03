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

struct chanSockW {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *c; /* chanSock_t item */
  int s;
};

static void
shutSockW(
  void *v
){
#define V ((struct chanSockW *)v)
  shutdown(V->s, SHUT_WR);
#undef V
}

static void *
chanSockW(
  void *v
){
#define V ((struct chanSockW *)v)
  chanSock_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockW, v);
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
  pthread_cleanup_pop(1); /* shutSockW(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

struct chanSockR {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chan_t *c; /* chanSock_t item */
  int s;
  unsigned int l;
};

static void
shutSockR(
  void *v
){
#define V ((struct chanSockR *)v)
  shutdown(V->s, SHUT_RD);
#undef V
}

static void *
chanSockR(
  void *v
){
#define V ((struct chanSockR *)v)
  chanSock_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))shutSockR, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  while ((m = V->a(0, sizeof (*m) + V->l - sizeof (m->b)))) {
    int f;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    if ((f = read(V->s, m->b, V->l)) > 0) {
      void *t;

      m->l = f;
      /* attempt to "right size" the item */
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
  pthread_cleanup_pop(1); /* shutSockR(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

int
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *r
 ,chan_t *w
 ,int s
 ,unsigned int l
){
  pthread_t t;

  if (((a || f) && (!a || !f)) || (!r && !w) || s < 0 || (r && !l))
    goto error;
  if (!a) {
    a = realloc;
    f = free;
  }
  if (w) {
    struct chanSockW *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->c = chanOpen(w);
    x->s = s;
    if (pthread_create(&t, 0, chanSockW, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  } else
    shutdown(s, SHUT_WR);
  if (r) {
    struct chanSockR *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->c = chanOpen(r);
    x->s = s;
    x->l = l;
    if (pthread_create(&t, 0, chanSockR, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  } else
    shutdown(s, SHUT_RD);
  return (1);
error:
  chanShut(r);
  chanShut(w);
  shutdown(s, SHUT_RDWR);
  return (0);
}
