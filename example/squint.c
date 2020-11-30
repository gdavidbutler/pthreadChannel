/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2020 G. David Butler <gdb@dbSystems.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "chan.h"
#include "chanFifo.h"

/*****************************************************
**     C / pthread / Channel implementation of      **
** M. Douglas McIlroy's "Squinting at Power Series" **
*****************************************************/

#define SZ 10 /* channel store size */

/*************************************************************************/

/* https://en.wikipedia.org/wiki/Rational_number */
typedef struct {
  long n; /* numerator */
  long d; /* denominator */
} rational;

/* https://en.wikipedia.org/wiki/Euclidean_algorithm */
static long
gcdI(
  long a
 ,long b
){
  long t;

  while (b) {
    t = b;
    b = a % b;
    a = t;
  }
  return (a);
}

/* new rational from integer numerator and integer denominator */
static rational *
newR(
  long n
 ,long d
){
  rational *r;
  long g;

  g = gcdI(n, d);
  if ((r = malloc(sizeof (*r)))) {
    r->n = n / g;
    r->d = d / g;
  } else
    fprintf(stderr, "newR OoR\n");
  return (r);
}

/* new rational from a rational */
static rational *
dupR(
  const rational *a
){
  rational *r;
  long g;

  g = gcdI(a->n, a->d);
  if ((r = malloc(sizeof (*r)))) {
    r->n = a->n / g;
    r->d = a->d / g;
  } else
    fprintf(stderr, "dupR OoR\n");
  return (r);
}

/* a/b + c/d = (ad + cb) / bd */
#if 0
static rational *
addR(
  const rational *a
 ,const rational *b
){
  long g;

  g = gcdI(a->d, b->d);
  return (newR(
   a->n * (b->d / g) + b->n * (a->d / g)
  ,a->d * (b->d / g)
  ));
}
#endif
static void
addToR(
  rational *a
 ,const rational *b
){
  long g;

  g = gcdI(a->d, b->d);
  a->n = a->n * (b->d / g) + b->n * (a->d / g);
  a->d = a->d * (b->d / g);
}

/* a/b - c/d = (ad - cb) / bd */
#if 0
static rational *
subR(
  const rational *a
 ,const rational *b
){
  long g;

  g = gcdI(a->d, b->d);
  return (newR(
   a->n * (b->d / g) - b->n * (a->d / g)
  ,b->d * (a->d / g)
  ));
}
#endif
#if 0
static void
subFromR(
  rational *a
 ,const rational *b
){
  long g;

  g = gcdI(a->d, b->d);
  a->n = a->n * (b->d / g) - b->n * (a->d / g);
  a->d = b->d * (a->d / g);
}
#endif

/* a/b . c/d = ac / bd */
#if 0
static rational *
mulR(
  const rational *a
 ,const rational *b
){
  long g1;
  long g2;

  g1 = gcdI(a->n, b->d);
  g2 = gcdI(b->n, a->d);
  return (newR(
   (a->n / g1) * (b->n / g2)
  ,(a->d / g2) * (b->d / g1)
  ));
}
#endif
static void
mulByR(
  rational *a
 ,const rational *b
){
  long g1;
  long g2;

  g1 = gcdI(a->n, b->d);
  g2 = gcdI(b->n, a->d);
  a->n = (a->n / g1) * (b->n / g2);
  a->d = (a->d / g2) * (b->d / g1);
}

/* neg a/b = -1/b */
#if 0
static rational *
negR(
  const rational *a
){
  return (newR(
   -a->n
  ,a->d
  ));
}
#endif
#if 0
static void
negOfR(
  rational *a
){
  a->n = -a->n;
}
#endif

/* reciprocal a/b = b/a */
#if 0
static rational *
rcpR(
  const rational *a
){
  return (newR(
   a->d
  ,a->n
  ));
}
#endif

static void
rcpOfR(
  rational *a
){
  long t;

  t = a->n;
  a->n = a->d;
  a->d = t;
}

/*************************************************************************/

/* split input stream to output streams */
struct splST {
  chanArr_t *a;
  unsigned int n;
};

static void *
splST(
void *v
#define V ((struct splST *)v)
){
  unsigned int n;
  void *t;

  n = 0;
  while (chanOne(0, V->n, V->a) == 1 && V->a->s == chanOsGet) {
    for (n = 1; n < V->n; ++n) {
      if (!(*((V->a + n)->v) = dupR(*V->a->v)))
        goto exit;
      (V->a + n)->o = chanOpPut;
    }
    V->a->o = chanOpSht;
    if (chanAll(0, V->n, V->a) != chanMsOp)
      break;
    free(*V->a->v);
    while (n)
      (V->a + --n)->o = chanOpSht;
    V->a->o = chanOpGet;
  }
exit:
  while (n)
    free(*((V->a + --n)->v));
  for (; n < V->n; ++n)
    free((V->a + n)->v);
  for (--n; n; --n)
    chanShut((V->a + n)->c), chanClose((V->a + n)->c);
  chanShut(V->a->c);
  while (chanOp(0, V->a->c, &t, chanOpGet) == chanOsGet)
    free(t);
  chanClose(V->a->c);
  free(V->a);
  free(v);
  return (0);
}
#undef V

static int
splS(
  chan_t *i
 ,chanArr_t *o
 ,unsigned int n
){
  struct splST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x)))
   || !(x->a = malloc((n + 1)  * sizeof (*x->a))))
    goto oom1;
  x->n = n + 1;
  for (n = 0; n < x->n; ++n)
    if (!((x->a + n)->v = malloc(sizeof (*x->a->v))))
      goto oom2;
  x->a->c = chanOpen(i);
  x->a->o = chanOpGet;
  for (n = 1; n < x->n; ++n) {
    (x->a + n)->c = chanOpen((o + n - 1)->c);
    (x->a + n)->o = chanOpSht;
  }
  if (pthread_create(&pt, 0, splST, x)) {
    void *v;

    for (n = 0; n < x->n; ++n)
      chanClose((x->a + n)->c);
oom2:
    while (n)
      free((x->a + --n)->v);
    free(x->a);
    n = x->n - 1;
oom1:
    free(x);
    fprintf(stderr, "splS OoR\n");
    while (n)
      chanShut((o + --n)->c);
    chanShut(i);
    while (chanOp(0, i, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* constant stream */
struct conST {
  chan_t *o;
  rational c;
};

static void *
conST(
void *v
#define V ((struct conST *)v)
){
  rational *o;

  while ((o = dupR(&V->c))) {
    if (chanOp(0, V->o, (void **)&o, chanOpPut) != chanOsPut) {
      free(o);
      break;
    }
    if (chanOp(0, V->o, 0, chanOpGet) != chanOsGet)
      break;
  }
  chanShut(V->o), chanClose(V->o);
  free(v);
  return (0);
}
#undef V

static int
conS(
  chan_t *o
 ,const rational *c
){
  struct conST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->o = chanOpen(o);
  x->c.n = c->n;
  x->c.d = c->d;
  if (pthread_create(&pt, 0, conST, x)) {
    chanClose(x->o);
oom:
    free(x);
    fprintf(stderr, "conS OoR\n");
    chanShut(o);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = Ft */
struct mulTT {
  chan_t *p;
  chan_t *f;
  rational t;
};

static void *
mulTT(
void *v
#define V ((struct mulTT *)v)
){
  rational *f;
  chanArr_t ga[2];
  chanArr_t pa[2];

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->p;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    mulByR(f, &V->t);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
  chanShut(V->p), chanClose(V->p);
  chanShut(V->f);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
mulT(
  chan_t *p
 ,chan_t *f
 ,rational *t
){
  struct mulTT *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->t.n = t->n;
  x->t.d = t->d;
  if (pthread_create(&pt, 0, mulTT, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->p);
oom:
    free(x);
    fprintf(stderr, "mulT OoR\n");
    chanShut(p);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream S = F + G */
struct addST {
  chan_t *s;
  chan_t *f;
  chan_t *g;
};

static void *
addST(
void *v
#define V ((struct addST *)v)
){
  rational *f;
  rational *g;
  chanArr_t ga[3];
  chanArr_t pa[3];

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->g;
  ga[1].v = (void **)&g;
  ga[1].o = chanOpGet;
  ga[2].c = V->s;
  ga[2].v = 0;
  ga[2].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = 0;
  pa[1].o = chanOpSht;
  pa[2].c = ga[2].c;
  pa[2].v = ga[0].v;
  pa[2].o = chanOpPut;
  while (chanAll(0, sizeof (ga) / sizeof (ga[0]), ga) == chanMsOp) {
    addToR(f, g);
    free(g);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 3 || pa[2].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[2].v = 0;
    pa[2].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 3 || pa[2].s != chanOsGet)
      break;
    pa[2].v = ga[0].v;
    pa[2].o = chanOpPut;
#endif
  }
  chanShut(V->s), chanClose(V->s);
  chanShut(V->f);
  chanShut(V->g);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  while (chanOp(0, V->g, (void **)&g, chanOpGet) == chanOsGet)
    free(g);
  chanClose(V->g);
  free(v);
  return (0);
}
#undef V

static int
addS(
  chan_t *s
 ,chan_t *f
 ,chan_t *g
){
  struct addST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->s = chanOpen(s);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, addST, x)) {
    void *v;

    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->s);
    free(x);
oom:
    fprintf(stderr, "addS OoR\n");
    chanShut(s);
    chanShut(f);
    chanShut(g);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    while (chanOp(0, g, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = x^n F */
struct xnST {
  chan_t *p;
  chan_t *f;
  unsigned long n;
};

static void *
xnST(
void *v
#define V ((struct xnST *)v)
){
  rational *f;
  chanArr_t ga[2];
  chanArr_t pa[2];

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->p;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  for (; V->n; --V->n) {
    if (!(f = newR(0, 1))
     || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      goto exit;
    }
  }
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->p), chanClose(V->p);
  chanShut(V->f);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
xnS(
  chan_t *p
 ,chan_t *f
 ,unsigned long n
){
  struct xnST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->n = n;
  if (pthread_create(&pt, 0, xnST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->p);
    free(x);
oom:
    fprintf(stderr, "xnS OoR\n");
    chanShut(p);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = FG */
struct mulST {
  chan_t *p;
  chan_t *f;
  chan_t *g;
};

static int
mulS(
  chan_t *p
 ,chan_t *f
 ,chan_t *g
);

static void *
mulST(
void *v
#define V ((struct mulST *)v)
){
  rational *f;
  rational *g;
  rational *x;
  chan_t *fG;
  chan_t *gF;
  chan_t *FG;
  chan_t *xFG;
  void *tv;
  chanArr_t ff[2];
  chanArr_t gg[2];
  chanArr_t ga[4];
  chanArr_t pa[4];
  rational cf;
  rational cg;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->g;
  ga[1].v = (void **)&g;
  ga[1].o = chanOpGet;
  ga[2].c = 0;
  ga[2].v = (void **)&x;
  ga[2].o = chanOpGet;
  ga[3].c = V->p;
  ga[3].v = 0;
  ga[3].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = 0;
  pa[1].o = chanOpSht;
  pa[2].c = ga[2].c;
  pa[2].v = 0;
  pa[2].o = chanOpSht;
  pa[3].c = ga[3].c;
  pa[3].v = ga[0].v;
  pa[3].o = chanOpPut;
  fG = gF = FG = xFG = 0;
  ff[0].c = ff[1].c = 0;
  gg[0].c = gg[1].c = 0;
  if (chanAll(0, sizeof (ga) / sizeof (ga[0]), ga) != chanMsOp)
    goto exit;
  cf.n = f->n;
  cf.d = f->d;
  cg.n = g->n;
  cg.d = g->d;
  mulByR(f, g);
  free(g);
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 4 || pa[3].s != chanOsPut) {
    free(f);
    goto exit;
  }
  /* demand channel begin */
  pa[3].v = 0;
  pa[3].o = chanOpGet;
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 4 || pa[3].s != chanOsGet)
    goto exit;
  pa[3].v = ga[0].v;
  pa[3].o = chanOpPut;
  /* demand channel end */
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(ff[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(ff[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(V->f, ff, sizeof (ff) / sizeof (ff[0])))
    goto exit;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(gg[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(gg[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(V->g, gg, sizeof (gg) / sizeof (gg[0]))) {
    chanShut(ff[0].c);
    chanShut(ff[1].c);
    while (chanOp(0, ff[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, ff[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(fG = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulT(fG, gg[0].c, &cf)) {
    chanShut(ff[0].c);
    chanShut(ff[1].c);
    chanShut(gg[0].c);
    chanShut(gg[1].c);
    while (chanOp(0, ff[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, ff[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  pa[0].c = ga[0].c = fG;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(gF = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulT(gF, ff[0].c, &cg)) {
    chanShut(ff[0].c);
    chanShut(ff[1].c);
    chanShut(gg[1].c);
    while (chanOp(0, ff[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, ff[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  pa[1].c = ga[1].c = gF;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(FG = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(FG, ff[1].c, gg[1].c)) {
    chanShut(ff[1].c);
    chanShut(gg[1].c);
    while (chanOp(0, ff[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  pa[2].c = ga[2].c = FG;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(xFG = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || xnS(xFG, FG, 1))
    goto exit;
  pa[2].c = ga[2].c = xFG;
  while (chanAll(0, sizeof (ga) / sizeof (ga[0]), ga) == chanMsOp) {
    addToR(f, g);
    free(g);
    addToR(f, x);
    free(x);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 4 || pa[3].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[3].v = 0;
    pa[3].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 4 || pa[3].s != chanOsGet)
      goto exit;
    pa[3].v = ga[0].v;
    pa[3].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->p), chanClose(V->p);
  chanShut(ff[0].c), chanClose(ff[0].c);
  chanShut(ff[1].c), chanClose(ff[1].c);
  chanShut(gg[0].c), chanClose(gg[0].c);
  chanShut(gg[1].c), chanClose(gg[1].c);
  chanShut(V->f);
  chanShut(V->g);
  chanShut(fG);
  chanShut(gF);
  chanShut(FG);
  chanShut(xFG);
  while (chanOp(0, ga[0].c, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  while (chanOp(0, ga[1].c, (void **)&g, chanOpGet) == chanOsGet)
    free(g);
  while (chanOp(0, ga[2].c, (void **)&x, chanOpGet) == chanOsGet)
    free(x);
  chanClose(V->f);
  chanClose(V->g);
  chanClose(fG);
  chanClose(gF);
  chanClose(FG);
  chanClose(xFG);
  free(v);
  return (0);
}
#undef V

static int
mulS(
  chan_t *p
 ,chan_t *f
 ,chan_t *g
){
  struct mulST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, mulST, x)) {
    void *v;

    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oom:
    chanShut(p);
    chanShut(f);
    chanShut(g);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    while (chanOp(0, g, &v, chanOpGet) == chanOsGet)
      free(v);
    fprintf(stderr, "mulS OoR\n");
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = dF/dx */
struct dffST {
  chan_t *p;
  chan_t *f;
};

static void *
dffST(
void *v
#define V ((struct dffST *)v)
){
  rational *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  rational n;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->p;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet)
    goto exit;
  free(f);
  n.n = 0;
  n.d = 1;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (!++n.n) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 && pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 && pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->p), chanClose(V->p);
  chanShut(V->f);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
dffS(
  chan_t *p
 ,chan_t *f
){
  struct dffST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, dffST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->p);
    free(x);
oom:
    fprintf(stderr, "dffS OoR\n");
    chanShut(p);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = integrate(0,inf)F(x) + c */
struct ntgST {
  chan_t *p;
  chan_t *f;
  rational *c;
};

static void *
ntgST(
void *v
#define V ((struct ntgST *)v)
){
  rational *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  rational n;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->p;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = (void **)&V->c;
  pa[1].o = chanOpPut;
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 && pa[1].s != chanOsPut) {
    free(V->c);
    goto exit;
  }
  pa[1].v = ga[0].v;
  n.n = 1;
  n.d = 0;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (!++n.d) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 && pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 && pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->p), chanClose(V->p);
  chanShut(V->f);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
ntgS(
  chan_t *p
 ,chan_t *f
 ,rational *c
){
  struct ntgST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x)))
   || !(x->c = dupR(c)))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, ntgST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->p);
    free(x->c);
oom:
    free(x);
    fprintf(stderr, "ntgS OoR\n");
    chanShut(p);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream S = F(G) */
struct sbtST {
  chan_t *s;
  chan_t *f;
  chan_t *g;
};

static int
sbtS(
  chan_t *s
 ,chan_t *f
 ,chan_t *g
);

static void *
sbtST(
void *v
#define V ((struct sbtST *)v)
){
  rational *f;
  chan_t *FG;
  chan_t *GF;
  void *tv;
  chanArr_t gg[2];
  chanArr_t ga[2];
  chanArr_t pa[2];

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->s;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  FG = GF = 0;
  gg[0].c = gg[1].c = 0;
  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet) {
    chanShut(V->g);
    while (chanOp(0, V->g, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
    free(f);
    chanShut(V->g);
    while (chanOp(0, V->g, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(gg[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(gg[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(V->g, gg, sizeof (gg) / sizeof (gg[0]))) {
    chanShut(V->g);
    while (chanOp(0, V->g, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  /* demand channel begin */
  pa[1].v = 0;
  pa[1].o = chanOpGet;
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet) {
    chanShut(gg[0].c);
    chanShut(gg[1].c);
    while (chanOp(0, gg[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  /* demand channel end */
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(FG = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || sbtS(FG, V->f, gg[0].c)) {
    chanShut(gg[0].c);
    chanShut(gg[1].c);
    while (chanOp(0, gg[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  ga[0].c = gg[1].c;
  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet) {
    chanShut(FG);
    while (chanOp(0, FG, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  free(f);
  ga[0].c = FG;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(GF = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(GF, gg[1].c, FG)) {
    chanShut(gg[1].c);
    while (chanOp(0, gg[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  pa[0].c = ga[0].c = GF;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      goto exit;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->s), chanClose(V->s);
  chanShut(V->g), chanClose(V->g);
  chanShut(gg[0].c), chanClose(gg[0].c);
  chanShut(gg[1].c), chanClose(gg[1].c);
  chanShut(V->f);
  chanShut(FG);
  chanShut(GF); while (chanOp(0, ga[0].c, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  chanClose(FG);
  chanClose(GF);
  free(v);
  return (0);
}
#undef V

static int
sbtS(
  chan_t *s
 ,chan_t *f
 ,chan_t *g
){
  struct sbtST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->s = chanOpen(s);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, sbtST, x)) {
    void *v;

    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->s);
    free(x);
oom:
    fprintf(stderr, "sbtS OoR\n");
    chanShut(s);
    chanShut(f);
    chanShut(g);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    while (chanOp(0, g, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream E = exp(F) */
struct expST {
  chan_t *e;
  chan_t *f;
};

static void *
expST(
void *v
#define V ((struct expST *)v)
){
  rational *x;
  chan_t *D;
  chan_t *X;
  chan_t *P;
  void *tv;
  chanArr_t xx[2];
  chanArr_t ga[2];
  chanArr_t pa[2];
  rational c;

  ga[0].c = V->f;
  ga[0].v = (void **)&x;
  ga[0].o = chanOpGet;
  ga[1].c = V->e;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  D = X = P = 0;
  xx[0].c = xx[1].c = 0;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(D = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || dffS(D, V->f))
    goto exit;
  ga[0].c = D;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(X = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(xx[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(xx[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(X, xx, sizeof (xx) / sizeof (xx[0]))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(P = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(P, xx[0].c, D))
    goto exit;
  ga[0].c = P;
  c.n = 1;
  c.d = 1;
  if (ntgS(X, P, &c))
    goto exit;
  ga[0].c = xx[1].c;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(x);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->e), chanClose(V->e);
  chanShut(X), chanClose(X);
  chanShut(xx[0].c), chanClose(xx[0].c);
  chanShut(V->f);
  chanShut(D);
  chanShut(P);
  chanShut(xx[1].c);
  while (chanOp(0, ga[0].c, (void **)&x, chanOpGet) == chanOsGet)
    free(x);
  chanClose(V->f);
  chanClose(D);
  chanClose(P);
  chanClose(xx[1].c);
  free(v);
  return (0);
}
#undef V

static int
expS(
  chan_t *e
 ,chan_t *f
){
  struct expST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->e = chanOpen(e);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, expST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->e);
    free(x);
oom:
    fprintf(stderr, "expS OoR\n");
    chanShut(e);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream R = 1/F */
struct rcpST {
  chan_t *r;
  chan_t *f;
};

static void *
rcpST(
void *v
#define V ((struct rcpST *)v)
){
  rational *f;
  chan_t *R;
  chan_t *M;
  void *tv;
  chanArr_t rr[2];
  chanArr_t ga[2];
  chanArr_t pa[2];
  rational n;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->r;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  R = M = 0;
  rr[0].c = rr[1].c = 0;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(R = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rr[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rr[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(R, rr, sizeof (rr) / sizeof (rr[0]))
   || chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet)
    goto exit;
  rcpOfR(f);
  n.n = -f->n;
  n.d = f->d;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(M = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(M, V->f, rr[0].c)) {
    free(f);
    goto exit;
  }
  ga[0].c = M;
  {
    chanArr_t ta[3];

    ta[0].c = R;
    ta[0].v = (void **)&f;
    ta[0].o = chanOpPut;
    ta[1].c = M;
    ta[1].v = 0;
    ta[1].o = chanOpSht;
    ta[1].c = V->r;
    ta[1].v = 0;
    ta[1].o = chanOpSht;
    if (chanOne(0, sizeof (ta) / sizeof (ta[0]), ta) != 1 || ta[0].s != chanOsPut) {
      free(f);
      chanShut(rr[1].c);
      while (chanOp(0, rr[1].c, (void **)&f, chanOpGet) == chanOsGet)
        free(f);
      goto exit;
    }
  }
  if (mulT(R, M, &n)) {
    chanShut(rr[1].c);
    while (chanOp(0, rr[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  ga[0].c = rr[1].c;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->r), chanClose(V->r);
  chanShut(rr[0].c), chanClose(rr[0].c);
  chanShut(R), chanClose(R);
  chanShut(V->f);
  chanShut(M);
  chanShut(rr[1].c);
  while (chanOp(0, ga[0].c, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  chanClose(M);
  chanClose(rr[1].c);
  free(v);
  return (0);
}
#undef V

static int
rcpS(
  chan_t *r
 ,chan_t *f
){
  struct rcpST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->r = chanOpen(r);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, rcpST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->r);
    free(x);
oom:
    fprintf(stderr, "rcpS OoR\n");
    chanShut(r);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream R = reversion: functional inverse of F, an R such that F(R(x)) = x */
struct revST {
  chan_t *r;
  chan_t *f;
};

static void *
revST(
void *v
#define V ((struct revST *)v)
){
  rational *f;
  rational *t;
  chan_t *R;
  chan_t *FR;
  chan_t *RR;
  chan_t *RRFR;
  void *tv;
  chanArr_t rr[3];
  chanArr_t rrr[2];
  chanArr_t ga[2];
  chanArr_t pa[2];
  rational n;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->r;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  R = FR = RR = RRFR = 0;
  rr[0].c = rr[1].c = 0;
  rrr[0].c = rrr[1].c = 0;
  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet)
    goto exit;
  t = f;
  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 1 || ga[0].s != chanOsGet) {
    free(t);
    goto exit;
  }
  rcpOfR(f);
  n.n = -f->n;
  n.d = f->d;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(R = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rr[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rr[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rr[2].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(R, rr, sizeof (rr) / sizeof (rr[0]))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(FR = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || sbtS(FR, V->f, rr[0].c)) {
    free(f);
    free(t);
    goto exit;
  }
  ga[0].c = FR;
  {
    chanArr_t ta[3];

    ta[0].c = R;
    ta[0].v = (void **)&t;
    ta[0].o = chanOpPut;
    ta[1].c = ga[0].c;
    ta[1].v = 0;
    ta[1].o = chanOpSht;
    ta[2].c = ga[1].c;
    ta[2].v = 0;
    ta[2].o = chanOpSht;
    if (chanOne(0, sizeof (ta) / sizeof (ta[0]), ta) != 1 || ta[0].s != chanOsPut) {
      free(f);
      free(t);
      goto exit;
    }
    ta[0].v = (void **)&f;
    if (chanOne(0, sizeof (ta) / sizeof (ta[0]), ta) != 1 || ta[0].s != chanOsPut) {
      free(f);
      chanShut(rr[1].c);
      chanShut(rr[2].c);
      while (chanOp(0, rr[1].c, (void **)&f, chanOpGet) == chanOsGet)
        free(f);
      while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
        free(f);
      goto exit;
    }
    ta[0].c = rr[1].c;
    ta[0].o = chanOpGet;
    if (chanOne(0, sizeof (ta) / sizeof (ta[0]), ta) != 1 || ta[0].s != chanOsGet) {
      chanShut(rr[1].c);
      chanShut(rr[2].c);
      while (chanOp(0, rr[1].c, (void **)&f, chanOpGet) == chanOsGet)
        free(f);
      while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
        free(f);
      goto exit;
    }
    free(f);
  }
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(rrr[0].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(rrr[1].c = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || splS(rr[1].c, rrr, sizeof (rrr) / sizeof (rrr[0]))) {
    chanShut(rr[1].c);
    chanShut(rr[2].c);
    while (chanOp(0, rr[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(RR = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(RR, rrr[0].c, rrr[1].c)) {
    chanShut(rrr[0].c);
    chanShut(rrr[1].c);
    chanShut(rr[2].c);
    while (chanOp(0, rrr[0].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, rrr[1].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(RRFR = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(RRFR, RR, FR)) {
    chanShut(RR);
    chanShut(rr[2].c);
    while (chanOp(0, RR, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  ga[0].c = RRFR;
  if (mulT(R, RRFR, &n)) {
    chanShut(rr[2].c);
    while (chanOp(0, rr[2].c, (void **)&f, chanOpGet) == chanOsGet)
      free(f);
    goto exit;
  }
  ga[0].c = rr[2].c;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->r), chanClose(V->r);
  chanShut(R), chanClose(R);
  chanShut(rr[0].c), chanClose(rr[0].c);
  chanShut(rr[1].c), chanClose(rr[1].c);
  chanShut(rrr[0].c), chanClose(rrr[0].c);
  chanShut(rrr[1].c), chanClose(rrr[1].c);
  chanShut(RR), chanClose(RR);
  chanShut(V->f);
  chanShut(FR);
  chanShut(RRFR);
  chanShut(rr[2].c);
  while (chanOp(0, ga[0].c, (void **)&t, chanOpGet) == chanOsGet)
    free(t);
  chanClose(V->f);
  chanClose(FR);
  chanClose(RRFR);
  chanClose(rr[2].c);
  free(v);
  return (0);
}
#undef V

static int
revS(
  chan_t *r
 ,chan_t *f
){
  struct revST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->r = chanOpen(r);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, revST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->r);
    free(x);
oom:
    fprintf(stderr, "revS OoR\n");
    chanShut(r);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream monomial substitution P = F(cx^n) */
struct msbtST {
  chan_t *p;
  chan_t *f;
  rational c;
  unsigned long n;
};

static void *
msbtST(
void *v
#define V ((struct msbtST *)v)
){
  rational *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  unsigned long i;

  ga[0].c = V->f;
  ga[0].v = (void **)&f;
  ga[0].o = chanOpGet;
  ga[1].c = V->p;
  ga[1].v = 0;
  ga[1].o = chanOpSht;
  pa[0].c = ga[0].c;
  pa[0].v = 0;
  pa[0].o = chanOpSht;
  pa[1].c = ga[1].c;
  pa[1].v = ga[0].v;
  pa[1].o = chanOpPut;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 1 && ga[0].s == chanOsGet) {
    mulByR(f, &V->c);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
      free(f);
      break;
    }
    for (i = 0; i < V->n; ++i)
      if (!(f = newR(0, 1))
       || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsPut) {
        free(f);
        goto exit;
      }
#if 0
    pa[1].v = 0;
    pa[1].o = chanOpGet;
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 2 || pa[1].s != chanOsGet)
      break;
    pa[1].v = ga[0].v;
    pa[1].o = chanOpPut;
#endif
  }
exit:
  chanShut(V->p), chanClose(V->p);
  chanShut(V->f);
  while (chanOp(0, V->f, (void **)&f, chanOpGet) == chanOsGet)
    free(f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
msbtS(
  chan_t *p
 ,chan_t *f
 ,rational *c
 ,unsigned long n
){
  struct msbtST *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oom;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->c.n = c->n;
  x->c.d = c->d;
  x->n = n > 1 ? n - 1 : 0;
  if (pthread_create(&pt, 0, msbtST, x)) {
    void *v;

    chanClose(x->f);
    chanClose(x->p);
oom:
    free(x);
    fprintf(stderr, "msbtS OoR\n");
    chanShut(p);
    chanShut(f);
    while (chanOp(0, f, &v, chanOpGet) == chanOsGet)
      free(v);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

static void
printS(
  chan_t *c
 ,unsigned int n
){
  rational *o;

  for (; n; --n) {
    if (chanOp(0, c, (void **)&o, chanOpGet) != chanOsGet)
      break;
    printf(" %ld/%ld", o->n, o->d),fflush(stdout);
    free(o);
  }
  putchar('\n'),fflush(stdout);
  chanShut(c);
  while (chanOp(0, c, (void **)&o, chanOpGet) == chanOsGet)
    free(o);
  chanClose(c);
}

int
main(
){
  chan_t *c1;
  chan_t *c2;
  chan_t *c3;
  chan_t *c4;
  void *tv;
  rational r1;
  rational r2;
  rational r3;

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1))
    goto exit;
  printf("conS:"),fflush(stdout);
  printS(c1, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulT(c2, c1, &r2))
    goto exit;
  printf("mulT:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c2, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c3 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || addS(c3, c1, c2))
    goto exit;
  printf("addS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || xnS(c2, c1, 1))
    goto exit;
  printf("xnS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c2, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c3 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || mulS(c3, c1, c2))
    goto exit;
  printf("mulS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || dffS(c2, c1))
    goto exit;
  printf("dffS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = 0, r2.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || ntgS(c2, c1, &r2))
    goto exit;
  printf("ntgS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c2, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c3 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || sbtS(c3, c1, c2))
    goto exit;
  printf("sbtS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || expS(c2, c1))
    goto exit;
  printf("expS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || rcpS(c2, c1))
    goto exit;
  printf("rcpS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || revS(c2, c1))
    goto exit;
  printf("revS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || msbtS(c2, c1, &r2, 2))
    goto exit;
  printf("msbtS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  r3.n = 0, r3.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || msbtS(c2, c1, &r2, 2)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c3 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || ntgS(c3, c2, &r3))
    goto exit;
  printf("ntg-msbt:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  r3.n = 0, r3.d = 1;
  if (!(tv = chanFifoStSa(realloc, free, SZ))
   || !(c1 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || conS(c1, &r1)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c2 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || msbtS(c2, c1, &r2, 2)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c3 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || ntgS(c3, c2, &r3)
   || !(tv = chanFifoStSa(realloc, free, SZ))
   || !(c4 = chanCreate(realloc, free, chanFifoStSi, tv, chanFifoStSd))
   || revS(c4, c3))
    goto exit;
  printf("tanS:"),fflush(stdout);
  printS(c4, 10);

exit:
  return (0);
}
