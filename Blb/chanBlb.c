/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2025 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of pthreadChannel
 *
 * pthreadChannel is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pthreadChannel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <time.h>
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

/**********************************************************/

struct ctxE { /* struct chanBlbEgrCtx with opaque names */
  void *(*ma)(void *, unsigned long);
  void (*mf)(void *);
  void *g;
  chan_t *c;
  void *x;
  unsigned int (*xf)(void *, const unsigned char *, unsigned int);
  void (*d)(void *);
  /* opaque[2] */
  void (*xc)(void *);
  volatile int *ex;
};

static void
finE(
  void *v
){
#define V ((struct ctxE *)v)
  *V->ex = 1;
  chanShut(V->c);
  chanClose(V->c);
  if (V->xc)
    V->xc(V->x);
  V->mf(v);
#undef V
}

static void *
nfE(
  void *v
){
#define V ((struct ctxE *)v)
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    unsigned int i;

    pthread_cleanup_push((void(*)(void*))V->mf, m);
    for (l = 0, i = 1; l < m->l && (i = V->xf(V->x, m->b + l, m->l - l)) > 0; l += i);
    pthread_cleanup_pop(1); /* V->mf(m) */
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  return (0);
#undef V
}

/**********************************************************/

struct ctxI { /* struct chanBlbIgrCtx with opaque names */
  void *(*ma)(void *, unsigned long);
  void (*mf)(void *);
  void *g;
  chan_t *c;
  void *x;
  unsigned int (*xf)(void *, unsigned char *, unsigned int);
  chanBlb_t *b;
  void (*d)(void *);
  /* opaque[2] */
  void (*xc)(void *);
  volatile int *ex;
};

static void
finI(
  void *v
){
#define V ((struct ctxI *)v)
  *V->ex = 1;
  chanShut(V->c);
  chanClose(V->c);
  V->mf(V->b);
  if (V->xc)
    V->xc(V->x);
  V->mf(v);
#undef V
}

unsigned int
chanBlbIgrBlb(
  void (*f)(void *)
 ,chanBlb_t **b
 ,void *d
 ,unsigned int l
){
  unsigned int i;
  unsigned int j;

  for (i = 0; i < l && i < (*b)->l; ++i)
    *((unsigned char *)d + i) = *((*b)->b + i);
  (*b)->l -= i;
  if ((*b)->l)
    for (j = 0; j < (*b)->l; ++j)
      *((*b)->b + j) = *((*b)->b + i + j);
  else {
    f(*b);
    *b = 0;
  }
  return (i);
}

static void *
nfI(
  void *v
){
#define V ((struct ctxI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int l;
  unsigned int i;

  pthread_cleanup_push((void(*)(void*))V->d, v);
  l = V->g ? (long)V->g : 65536;
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  while ((m = V->ma(0, chanBlb_tSize(l)))) {
    void *t;

    pthread_cleanup_push((void(*)(void*))V->mf, m);
    if (V->b) {
      if ((i = chanBlbIgrBlb(V->mf, &V->b, m->b, l)) < l)
        i += V->xf(V->x, m->b + i, l - i);
    } else
      i = V->xf(V->x, m->b, l);
    pthread_cleanup_pop(0); /* V->mf(m) */
    if (!i)
      break;
    m->l = i;
    if ((t = V->ma(m, chanBlb_tSize(m->l))))
      m = t;
    pthread_cleanup_push((void(*)(void*))V->mf, m);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* V->mf(m) */
    if (!i)
      break;
  }
  V->mf(m);
  pthread_cleanup_pop(1); /* V->d(v) */
  return (0);
#undef V
}

/**********************************************************/

struct monCtx {
  void (*mf)(void *);
  chan_t *cE;
  chan_t *cI;
  pthread_t tE;
  pthread_t tI;
  volatile int exE;
  volatile int exI;
  void *f;
  void (*fc)(void *);
};

static void
monFin(
  void *v
#define V ((struct monCtx *)v)
){
  if (V->cE) {
    if (!V->exE)
      pthread_cancel(V->tE);
    pthread_join(V->tE, 0);
    chanClose(V->cE);
  }
  if (V->cI) {
    if (!V->exI)
      pthread_cancel(V->tI);
    pthread_join(V->tI, 0);
    chanClose(V->cI);
  }
  if (V->fc)
    V->fc(V->f);
  V->mf(v);
#undef V
}

static void *
monT(
  void *v
#define V ((struct monCtx *)v)
){
  chanArr_t p[2];
  unsigned int n;
  unsigned int i;
  struct timespec ts;

  pthread_cleanup_push(monFin, v);
  p[0].c = V->cE; p[0].v = 0; p[0].o = chanOpSht;
  p[1].c = V->cI; p[1].v = 0; p[1].o = chanOpSht;
  while (p[0].c || p[1].c) {
    if (!(n = chanOne(0, 2, p)))
      break;
    --n;
    if (p[n].s == chanOsSht)
      p[n].c = 0;
  }
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  for (i = 0; i < 1800; ++i) {
    if ((!V->cE || V->exE) && (!V->cI || V->exI))
      break;
    nanosleep(&ts, 0);
  }
  pthread_cleanup_pop(1);
  return (0);
#undef V
}

/**********************************************************/

int
chanBlb(
  void *(*ma)(void *, unsigned long)
 ,void (*mf)(void *)

 ,chan_t *e
 ,void *ot
 ,unsigned int (*otf)(void *, const unsigned char *, unsigned int)
 ,void (*otc)(void *)
 ,void *eg
 ,void *(*fe)(struct chanBlbEgrCtx *)

 ,chan_t *i
 ,void *in
 ,unsigned int (*inf)(void *, unsigned char *, unsigned int)
 ,void (*inc)(void *)
 ,void *ig
 ,void *(*fi)(struct chanBlbIgrCtx *)
 ,chanBlb_t *b

 ,void *f
 ,void (*fc)(void *)

 ,pthread_attr_t *a
){
  pthread_t tE;
  pthread_t tI;
  struct monCtx *m;
  pthread_t t;
  int hE;
  int hI;
  int hM;

  hE = 0;
  hI = 0;
  hM = 0;
  if (!ma || !mf
   || (!e && !i)
   || (e && !otf)
   || (i && !inf)
  )
    goto error;
  mf(ma(0, 1)); /* force exception here and now */
  if (!(m = ma(0, sizeof (*m))))
    goto error;
  hM = 1;
  m->mf = mf;
  m->cE = 0;
  m->cI = 0;
  m->exE = 0;
  m->exI = 0;
  m->f = f;
  m->fc = fc;

  if (e) {
    struct ctxE *x;

    if (!(x = ma(0, sizeof (*x))))
      goto error;
    x->ma = ma;
    x->mf = mf;
    x->c = chanOpen(e);
    x->x = ot;
    x->xf = otf;
    x->xc = otc;
    x->d = finE;
    x->g = eg;
    x->ex = &m->exE;
    if (pthread_create(&tE, a, fe ? (void *(*)(void *))fe : nfE, x)) {
      chanClose(x->c);
      mf(x);
      goto error;
    }
    hE = 1;
    m->tE = tE;
    otc = 0;
  } else if (otc)
    otc(ot);
  if (i) {
    struct ctxI *x;

    if (!(x = ma(0, sizeof (*x))))
      goto error;
    x->ma = ma;
    x->mf = mf;
    x->c = chanOpen(i);
    x->x = in;
    x->xf = inf;
    x->xc = inc;
    x->d = finI;
    x->g = ig;
    x->b = b;
    x->ex = &m->exI;
    if (pthread_create(&tI, a, fi ? (void *(*)(void *))fi : nfI, x)) {
      chanClose(x->c);
      mf(x);
      goto error;
    }
    hI = 1;
    m->tI = tI;
    b = 0;
    inc = 0;
  } else if (inc)
    inc(in);
  m->cE = chanOpen(e);
  m->cI = chanOpen(i);
  if (pthread_create(&t, a, monT, m)) {
    chanClose(m->cE);
    chanClose(m->cI);
    goto error;
  }
  pthread_detach(t);
  return (1);
error:
  if (hE) {
    chanShut(e);
    pthread_join(tE, 0);
  }
  if (hI) {
    chanShut(i);
    pthread_join(tI, 0);
  }
  if (hM)
    mf(m);
  chanShut(e);
  chanShut(i);
  if (otc)
    otc(ot);
  if (inc)
    inc(in);
  if (fc)
    fc(f);
  mf(b);
  return (0);
}
