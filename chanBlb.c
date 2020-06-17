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

#include <stdlib.h>     /* to support 0,0 to indicate realloc() and free() */
#include <unistd.h>     /* for read(), write() and close() */
#include <sys/socket.h> /* for shutdown() */
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"

unsigned int
chanBlb_tSize(
  unsigned int l
){
  return (
     l > sizeof (chanBlb_t) - (unsigned long)&((chanBlb_t *)0)->b
   ? l + (unsigned long)&((chanBlb_t *)0)->b
   : sizeof (chanBlb_t)
  );
}

struct chanBlbW {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  void (*d)(void *);
  chan_t *c;
  int s;
};

static void *
chanNfW(
  void *v
){
#define V ((struct chanBlbW *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    int i;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    for (l = 0, i = 1; l < m->l && (i = write(V->s, m->b + l, m->l - l)) > 0; l += i);
    pthread_cleanup_pop(1); /* V->f(m) */
    if (i <= 0)
      break;
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void *
chanNsW(
  void *v
){
#define V ((struct chanBlbW *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    int i;
    char b[16];

    i = sizeof (b) - 1;
    b[i] = ':';
    l = m->l;
    do {
      b[--i] = l % 10 + '0';
      l /= 10;
    } while (i && l);
    pthread_cleanup_push((void(*)(void*))V->f, m);
    if (!l
     && (l = sizeof (b) - i, i = write(V->s, &b[i], l)) > 0
     && (unsigned int)i == l)
      for (l = 0; l < m->l && (i = write(V->s, m->b + l, m->l - l)) > 0; l += i);
    else
      i = 0;
    pthread_cleanup_pop(1); /* V->f(m) */
    if (i <= 0
     || write(V->s, ",", 1) <= 0)
      break;
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

struct chanBlbR {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  void (*d)(void *);
  chan_t *c;
  int s;
  unsigned int l;
};

static void *
chanNfR(
  void *v
){
#define V ((struct chanBlbR *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  while ((m = V->a(0, chanBlb_tSize(V->l)))) {
    void *t;
    int i;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    i = read(V->s, m->b, V->l);
    pthread_cleanup_pop(0); /* V->f(m) */
    if (i <= 0)
      break;
    m->l = i;
    if ((t = V->a(m, chanBlb_tSize(m->l))))
      m = t;
    pthread_cleanup_push((void(*)(void*))V->f, m);
    i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* V->f(m) */
    if (!i)
      break;
  }
  V->f(m);
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void *
chanNsR(
  void *v
){
#define V ((struct chanBlbR *)v)
  chanBlb_t *m;
  chanPoll_t p[1];
  int r;
  int f;
  char b[16];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  r = 0;
  while ((f = read(V->s, b + r, sizeof (b) - r)) > 0) {
    unsigned int l;
    int i;

    f += r;
    for (i = 0, l = 0; i < f && b[i] <= '9' && b[i] >= '0'; ++i)
      l = l * 10 + (b[i] - '0');
    if (i == f
     || b[i++] != ':'
     || (V->l && V->l < l)
     || !(m = V->a(0, chanBlb_tSize(l))))
      break;
    m->l = l;
    for (l = 0; l < m->l && i < f;)
      m->b[l++] = b[i++];
    for (r = 0; i < f;)
      b[r++] = b[i++];
    pthread_cleanup_push((void(*)(void*))V->f, m);
    for (; l < m->l && (i = read(V->s, m->b + l, m->l - l)) > 0; l += i);
    if (i > 0) {
      if ((r && b[--r] == ',')
       || (!r && (i = read(V->s, b, 1)) > 0 && b[0] == ','))
        i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      else
        i = 0;
    }
    pthread_cleanup_pop(0); /* V->f(m) */
    if (i <= 0) {
      V->f(m);
      break;
    }
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void
shutSockW(
  void *v
){
#define V ((struct chanBlbW *)v)
  shutdown(V->s, SHUT_WR);
#undef V
}

static void
shutSockR(
  void *v
){
#define V ((struct chanBlbR *)v)
  shutdown(V->s, SHUT_RD);
#undef V
}

int
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *r
 ,chan_t *w
 ,int s
 ,chanBlbFrm_t m
 ,int g
){
  pthread_t t;

  if (((a || f) && (!a || !f))
   || (!r && !w)
   || s < 0
   || m < chanBlbFrmNf
   || m > chanBlbFrmNs
   || (m == chanBlbFrmNf && g <= 0)
   || (m == chanBlbFrmNs && g < 0))
    goto error;
  if (!a) {
    a = realloc;
    f = free;
  }
  if (w) {
    struct chanBlbW *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = shutSockW;
    x->c = chanOpen(w);
    x->s = s;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsW
     :chanNfW, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  if (r) {
    struct chanBlbR *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = shutSockR;
    x->c = chanOpen(r);
    x->s = s;
    x->l = g;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsR
     :chanNfR, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  return (1);
error:
  return (0);
}

static void
closeFdW(
  void *v
){
#define V ((struct chanBlbW *)v)
  close(V->s);
#undef V
}

static void
closeFdR(
  void *v
){
#define V ((struct chanBlbR *)v)
  close(V->s);
#undef V
}

int
chanPipe(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *r
 ,chan_t *w
 ,int d
 ,int e
 ,chanBlbFrm_t m
 ,int g
){
  pthread_t t;

  if (((a || f) && (!a || !f))
   || (!r && !w)
   || (r && d < 0)
   || (w && e < 0)
   || m < chanBlbFrmNf
   || m > chanBlbFrmNs
   || (m == chanBlbFrmNf && g <= 0)
   || (m == chanBlbFrmNs && g < 0))
    goto error;
  if (!a) {
    a = realloc;
    f = free;
  }
  if (w) {
    struct chanBlbW *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = closeFdW;
    x->c = chanOpen(w);
    x->s = e;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsW
     :chanNfW, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  if (r) {
    struct chanBlbR *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = closeFdR;
    x->c = chanOpen(r);
    x->s = d;
    x->l = g;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsR
     :chanNfR, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  return (1);
error:
  return (0);
}
