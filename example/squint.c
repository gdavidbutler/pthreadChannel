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
#include <string.h>
#include <pthread.h>
#include "chan.h"
#include "chanFifo.h"

/*****************************************************
**     C / pthread / Channel implementation of      **
** M. Douglas McIlroy's "Squinting at Power Series" **
*****************************************************/

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

/* new rational from numerator and denominator */
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

/* new rational from rational */
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

  ga[0].c = V->p;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;

  pa[0].c = V->p;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;

  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    mulByR(f, &V->t);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
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

  ga[0].c = V->s;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;
  ga[2].c = V->g;
  ga[2].v = (void **)&g;
  ga[2].o = chanOpGet;

  pa[0].c = V->s;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;
  pa[2].c = V->g;
  pa[2].v = 0;
  pa[2].o = chanOpSht;

  while (chanAll(0, sizeof (ga) / sizeof (ga[0]), ga) == chanAlOp) {
    addToR(f, g);
    free(g);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
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

  ga[0].c = V->p;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;

  pa[0].c = V->p;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;

  for (; V->n; --V->n) {
    if (!(f = newR(0, 1))
     || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      goto exit;
    }
  }
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
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

/* recursive forward declaration */
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
  enum ch {
   P = 0, F, G, F0, F1, G0, G1, Fg, Gf, FG, xFG, chCnt
  };
  rational *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F, G */
  chanArr_t pa1[chCnt]; /* put P */
  chanArr_t pa2[chCnt]; /* put F0, F1, G0, G1 */
  chanArr_t ga2[chCnt]; /* get Fg, Gf, xFG */
  rational f;
  rational g;
  unsigned int i;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  memset(ga2, 0, sizeof (ga2));
  ga1[P].c = V->p;
  ga1[F].c = V->f;
  ga1[G].c = V->g;
  for (i = 0; i < chCnt; ++i) {
    void *tv;

    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, 0)))
      goto exit;
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));
  memcpy(pa2, ga1, sizeof (pa2));
  memcpy(ga2, ga1, sizeof (ga2));

  ga1[F].o = ga1[G].o = chanOpGet;
  pa1[P].o = chanOpPut;
  if (chanAll(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != chanAlOp)
    goto exit;
  f.n = r[F]->n;
  f.d = r[F]->d;
  g.n = r[G]->n;
  g.d = r[G]->d;
  r[P] = r[F];
  mulByR(r[P], r[G]);
  free(r[G]);
  if (chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != P + 1 || pa1[P].s != chanOsPut) {
    free(r[P]);
    goto exit;
  }
  /* "demand" channel begin (check for a get before recursing) */
  pa1[P].v = 0;
  if (chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != P + 1 || pa1[P].s != chanOsPut)
    goto exit;
  pa1[P].v = (void **)&r[P];
  /* "demand" channel end */
  if (mulT(ga2[Fg].c, pa2[F0].c, &g)
   || mulT(ga2[Gf].c, pa2[G0].c, &f)
   || mulS(ga2[FG].c, pa2[F1].c, pa2[G1].c)
   || xnS(ga2[xFG].c, pa2[FG].c, 1))
    goto exit;
  pa2[F0].o = pa2[F1].o = pa2[G0].o = pa2[G1].o = chanOpPut;
  ga2[Fg].o = ga2[Gf].o = ga2[xFG].o = chanOpGet;
  while (chanAll(0, sizeof (ga1) / sizeof (ga1[0]), ga1) == chanAlOp) {
    r[F0] = r[F];
    r[G0] = r[G];
    r[G1] = 0;
    if (!(r[F1] = dupR(r[F]))
     || !(r[G1] = dupR(r[G]))
     || chanAll(0, sizeof (pa2) / sizeof (pa2[0]), pa2) != chanAlOp) {
      free(r[F0]);
      free(r[F1]);
      free(r[G0]);
      free(r[G1]);
      break;
    }
    if (chanAll(0, sizeof (ga2) / sizeof (ga2[0]), ga2) != chanAlOp)
      break;
    r[P] = r[Fg];
    addToR(r[P], r[Gf]);
    free(r[Gf]);
    addToR(r[P], r[xFG]);
    free(r[xFG]);
    if (chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != P + 1 || pa1[P].s != chanOsPut) {
      free(r[P]);
      break;
    }
  }
exit:
  for (i = 0; i < chCnt; ++i)
    if (pa1[i].o == chanOpPut)
      chanShut(pa1[i].c), chanClose(pa1[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (pa2[i].o == chanOpPut)
      chanShut(pa2[i].c), chanClose(pa2[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet)
      chanShut(ga1[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga2[i].o != chanOpGet)
      chanShut(ga2[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet) {
      while (chanOp(0, ga1[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga1[i].c);
    }
  for (i = 0; i < chCnt; ++i)
    if (ga2[i].o == chanOpGet) {
      while (chanOp(0, ga2[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga2[i].c);
    }
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

  ga[0].c = V->p;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;

  pa[0].c = V->p;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;

  if (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) != 2 || ga[1].s != chanOsGet)
    goto exit;
  free(f);
  n.n = 0;
  n.d = 1;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    if (!++n.n) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 && pa[0].s != chanOsPut) {
      free(f);
      break;
    }
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

  ga[0].c = V->p;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;

  pa[0].c = V->p;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;

  f = V->c;
  if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
    free(f);
    goto exit;
  }
  n.n = 1;
  n.d = 0;
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    if (!++n.d) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 && pa[0].s != chanOsPut) {
      free(f);
      break;
    }
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

/* recursive forward declaration */
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
  enum ch {
   S = 0, F, G, G0, G1, FG, chCnt
  };
  rational *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then G */
  chanArr_t pa1[chCnt]; /* put S then G0, G1 */
  unsigned int i;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  ga1[S].c = V->s;
  ga1[F].c = V->f;
  ga1[G].c = V->g;
  for (i = 0; i < chCnt; ++i) {
    void *tv;

    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, 0)))
      goto exit;
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));

  ga1[F].o = chanOpGet;
  pa1[S].o = chanOpPut;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet) {
    ga1[G].o = chanOpGet;
    goto exit;
  }
  ga1[G].o = chanOpGet;
  r[S] = r[F];
  if (chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != S + 1 || pa1[S].s != chanOsPut) {
    free(r[S]);
    goto exit;
  }
  /* "demand" channel begin (check for a get before recursing) */
  pa1[S].v = 0;
  if (chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != S + 1 || pa1[S].s != chanOsPut)
    goto exit;
  pa1[S].v = (void **)&r[S];
  /* "demand" channel end */
  if (sbtS(ga1[FG].c, pa1[F].c, pa1[G0].c)
   || mulS(ga1[S].c, pa1[G1].c, pa1[FG].c))
    goto exit;
  ga1[F].o = chanOpSht;
  pa1[S].o = chanOpSht;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != G + 1 || ga1[G].s != chanOsGet)
    goto exit;
  r[G0] = r[G];
  pa1[G0].o = pa1[G1].o = chanOpPut;
  while (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) == G + 1 && ga1[G].s == chanOsGet) {
    if (!(r[G1] = dupR(r[G]))
     || chanAll(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != chanAlOp) {
      free(r[G1]);
      free(r[G]);
      break;
    }
    r[G0] = r[G];
  }
  free(r[G0]);
exit:
  for (i = 0; i < chCnt; ++i)
    if (pa1[i].o == chanOpPut)
      chanShut(pa1[i].c), chanClose(pa1[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet)
      chanShut(ga1[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet) {
      while (chanOp(0, ga1[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga1[i].c);
    }
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
  enum ch {
   X0 = 0, F, D, P, X, X1, chCnt
  };
  rational *r[chCnt];
  chanArr_t ga1[chCnt]; /* get X */
  chanArr_t pa1[chCnt]; /* put X0, X1 */
  unsigned int i;
  rational c;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  ga1[F].c = V->f;
  ga1[X0].c = V->e;
  for (i = 0; i < chCnt; ++i) {
    void *tv;

    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, 0)))
      goto exit;
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));

  c.n = 1;
  c.d = 1;
  ga1[F].o = chanOpGet;
  pa1[X0].o = chanOpPut;
  if (dffS(ga1[D].c, pa1[F].c)
   || mulS(ga1[P].c, pa1[X1].c, pa1[D].c)
   || ntgS(ga1[X].c, pa1[P].c, &c))
    goto exit;
  ga1[F].o = chanOpSht;
  ga1[X].o = chanOpGet;
  pa1[X1].o = chanOpPut;
  while ((i = chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1)) == X + 1 && ga1[X].s == chanOsGet) {
    r[X0] = r[X];
    if (!(r[X1] = dupR(r[X]))
     || chanAll(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != chanAlOp) {
      free(r[X0]);
      free(r[X1]);
      break;
    }
  }
exit:
  for (i = 0; i < chCnt; ++i)
    if (pa1[i].o == chanOpPut)
      chanShut(pa1[i].c), chanClose(pa1[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet)
      chanShut(ga1[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet) {
      while (chanOp(0, ga1[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga1[i].c);
    }
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
  enum ch {
   R = 0, F, M, RM, RT, chCnt
  };
  rational *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then RT */
  chanArr_t pa1[chCnt]; /* put R */
  chanArr_t pa2[chCnt]; /* put RM */
  unsigned int i;
  rational n;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  ga1[R].c = V->r;
  ga1[F].c = V->f;
  for (i = 0; i < chCnt; ++i) {
    void *tv;

    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, 0)))
      goto exit;
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));
  memcpy(pa2, ga1, sizeof (pa2));

  ga1[F].o = chanOpGet;
  pa1[R].o = chanOpPut;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet)
    goto exit;
  ga1[F].o = chanOpSht;
  rcpOfR(r[F]);
  n.n = -r[F]->n;
  n.d = r[F]->d;
  if (!(r[R] = dupR(r[F]))
   || chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != R + 1 || pa1[R].s != chanOsPut) {
    free(r[R]);
    free(r[F]);
    goto exit;
  }
  r[RM] = r[F];
  pa2[RM].o = chanOpPut;
  if (chanOne(0, sizeof (pa2) / sizeof (pa2[0]), pa2) != RM + 1 || pa2[RM].s != chanOsPut) {
    free(r[RM]);
    goto exit;
  }
  if (mulS(ga1[M].c, pa1[F].c, pa1[RM].c)
   || mulT(ga1[RT].c, pa1[M].c, &n))
    goto exit;
  ga1[RT].o = chanOpGet;
  while (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) == RT + 1 && ga1[RT].s == chanOsGet) {
    if (!(r[R] = dupR(r[RT]))
     || chanOne(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != R + 1 || pa1[R].s != chanOsPut) {
      free(r[R]);
      free(r[RT]);
      break;
    }
    r[RM] = r[RT];
    if (chanOne(0, sizeof (pa2) / sizeof (pa2[0]), pa2) != RM + 1 || pa2[RM].s != chanOsPut) {
      free(r[RM]);
      break;
    }
  }
exit:
  for (i = 0; i < chCnt; ++i)
    if (pa1[i].o == chanOpPut)
      chanShut(pa1[i].c), chanClose(pa1[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (pa2[i].o == chanOpPut)
      chanShut(pa2[i].c), chanClose(pa2[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet)
      chanShut(ga1[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet) {
      while (chanOp(0, ga1[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga1[i].c);
    }
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
  enum ch {
   R0 = 0, F, R1, FR1, RB2, RB0, RB1, RB2FR1, R, chCnt
  };
  rational *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then R */
  chanArr_t pa1[chCnt]; /* put R0 R1 */
  chanArr_t pa2[chCnt]; /* put RB0 RB1 */
  unsigned int i;
  rational n;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  ga1[R0].c = V->r;
  ga1[F].c = V->f;
  for (i = 0; i < chCnt; ++i) {
    void *tv;

    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, 0)))
      goto exit;
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));
  memcpy(pa2, ga1, sizeof (pa2));

  ga1[F].o = chanOpGet;
  pa1[R0].o = chanOpPut;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet)
    goto exit;
  r[R0] = r[F];
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet) {
    free(r[R0]);
    goto exit;
  }
  rcpOfR(r[F]);
  n.n = -r[F]->n;
  n.d = r[F]->d;
  if (sbtS(ga1[FR1].c, pa1[F].c, pa1[R1].c)
   || mulS(ga1[RB2].c, pa2[RB0].c, pa2[RB1].c)
   || mulS(ga1[RB2FR1].c, pa2[RB2].c, pa1[FR1].c)
   || mulT(ga1[R].c, pa1[RB2FR1].c, &n)) {
    free(r[R0]);
    free(r[F]);
    goto exit;
  }
  ga1[F].o = chanOpSht;
  ga1[R].o = chanOpGet;
  pa1[R1].o = chanOpPut;
  if (!(r[R1] = dupR(r[R0]))
   || chanAll(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != chanAlOp) {
    free(r[R1]);
    free(r[R0]);
    free(r[F]);
    goto exit;
  }
  pa2[RB0].o = pa2[RB1].o = chanOpPut;
  r[R] = r[F];
  do {
    r[RB1] = 0;
    if (!(r[RB0] = dupR(r[R]))
     || !(r[RB1] = dupR(r[R]))
     || chanAll(0, sizeof (pa2) / sizeof (pa2[0]), pa2) != chanAlOp) {
      free(r[RB1]);
      free(r[RB0]);
      free(r[R]);
      break;
    }
    r[R0] = r[R];
    if (!(r[R1] = dupR(r[R]))
     || chanAll(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != chanAlOp) {
      free(r[R1]);
      free(r[R0]);
      break;
    }
  } while (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) == R + 1 && ga1[R].s == chanOsGet);
exit:
  for (i = 0; i < chCnt; ++i)
    if (pa1[i].o == chanOpPut)
      chanShut(pa1[i].c), chanClose(pa1[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (pa2[i].o == chanOpPut)
      chanShut(pa2[i].c), chanClose(pa2[i].c);;
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet)
      chanShut(ga1[i].c);
  for (i = 0; i < chCnt; ++i)
    if (ga1[i].o == chanOpGet) {
      while (chanOp(0, ga1[i].c, (void **)&r[0], chanOpGet) == chanOsGet)
        free(r[0]);
      chanClose(ga1[i].c);
    }
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

  ga[0].c = V->p;
  ga[0].v = 0;
  ga[0].o = chanOpSht;
  ga[1].c = V->f;
  ga[1].v = (void **)&f;
  ga[1].o = chanOpGet;

  pa[0].c = V->p;
  pa[0].v = (void **)&f;
  pa[0].o = chanOpPut;
  pa[1].c = V->f;
  pa[1].v = 0;
  pa[1].o = chanOpSht;

  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    mulByR(f, &V->c);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
    for (i = 0; i < V->n; ++i)
      if (!(f = newR(0, 1))
       || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
        free(f);
        goto exit;
      }
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

  chanInit(realloc, free);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1))
    goto exit;
  printf("conS:"),fflush(stdout);
  printS(c1, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || mulT(c2, c1, &r2))
    goto exit;
  printf("mulT:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, 0))
   || addS(c3, c1, c2))
    goto exit;
  printf("addS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || xnS(c2, c1, 1))
    goto exit;
  printf("xnS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, 0))
   || mulS(c3, c1, c2))
    goto exit;
  printf("mulS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || dffS(c2, c1))
    goto exit;
  printf("dffS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = 0, r2.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || ntgS(c2, c1, &r2))
    goto exit;
  printf("ntgS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, 0))
   || sbtS(c3, c1, c2))
    goto exit;
  printf("sbtS:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || expS(c2, c1))
    goto exit;
  printf("expS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || rcpS(c2, c1))
    goto exit;
  printf("rcpS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || revS(c2, c1))
    goto exit;
  printf("revS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || msbtS(c2, c1, &r2, 2))
    goto exit;
  printf("msbtS:"),fflush(stdout);
  printS(c2, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  r3.n = 0, r3.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || msbtS(c2, c1, &r2, 2)
   || !(c3 = chanCreate(0, 0, 0))
   || ntgS(c3, c2, &r3))
    goto exit;
  printf("ntg-msbt:"),fflush(stdout);
  printS(c3, 10);

  r1.n = 1, r1.d = 1;
  r2.n = -1, r2.d = 1;
  r3.n = 0, r3.d = 1;
  if (!(c1 = chanCreate(0, 0, 0))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, 0))
   || msbtS(c2, c1, &r2, 2)
   || !(c3 = chanCreate(0, 0, 0))
   || ntgS(c3, c2, &r3)
   || !(c4 = chanCreate(0, 0, 0))
   || revS(c4, c3))
    goto exit;
  printf("tanS:"),fflush(stdout);
  printS(c4, 10);

exit:
  return (0);
}
