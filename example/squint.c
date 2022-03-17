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

/*****************************************************
**     C / pthread / Channel implementation of      **
** M. Douglas McIlroy's "Squinting at Power Series" **
**                                                  **
**    This implementation produces the stream of    **
**  coefficients of the tangent series on page 23.  **
**  (Note: "fanout", "queuing" and "demand" are no  **
**      are no longer technical difficulties.)      **
**                                                  **
**                   In Postscript                  **
**  [https://cs.dartmouth.edu/~doug/squint.ps.gz]   **
**                                                  **
**                      In PDF                      **
**    [https://swtch.com/~rsc/thread/squint.pdf]    **
**                                                  **
**                  A Later paper                   **
**    [https://cs.dartmouth.edu/~doug/music.pdf]    **
**                                                  **
**                       NOTE                       **
**       This was developed to test chanAll()       **
**           using a default store of one.          **
**    Larger stores will NOT increase performance   **
**            because channel back presure          **
**          limits creating unused elements.        **
*****************************************************/

/*************************************************************************/

/* https://en.wikipedia.org/wiki/Rational_number */
typedef struct {
  long n; /* numerator */
  long d; /* denominator */
} rat_t;

/* rational from numerator and denominator */
static rat_t *
newR(
  long n
 ,long d
){
  rat_t *r;

  if ((r = malloc(sizeof (*r)))) {
    r->n = n;
    r->d = d;
  } else
    fprintf(stderr, "newR OoR\n");
  return (r);
}

static void
setR(
  rat_t *r
 ,long n
 ,long d
){
  r->n = n;
  r->d = d;
}

/* rational from rational */
static rat_t *
dupR(
  const rat_t *a
){
  rat_t *r;

  if ((r = malloc(sizeof (*r)))) {
    r->n = a->n;
    r->d = a->d;
  } else
    fprintf(stderr, "dupR OoR\n");
  return (r);
}

static void
cpyR(
  rat_t *a
 ,const rat_t *b
){
  a->n = b->n;
  a->d = b->d;
}

/* https://en.wikipedia.org/wiki/Euclidean_algorithm#Method_of_least_absolute_remainders */
static long
gcdI(
  long a
 ,long b
){
  long r;
  long t;

  if ((a < 0 ? -a : a) < (b < 0 ? -b : b)) {
    while (a) {
      r = b % a;
      t = a > r ? a - r : r - a;
      b = a;
      a = r < t ? r : t;
    }
    return (b);
  } else {
    while (b) {
      r = a % b;
      t = b > r ? b - r : r - b;
      a = b;
      b = r < t ? r : t;
    }
    return (a);
  }
}

/* a/b + c/d = (ad + cb) / bd */
#if 0
static rat_t *
addR(
  const rat_t *a
 ,const rat_t *b
){
  long g;
  long t;

  if (!(g = gcdI(a->d, b->d)))
    return (0);
  t = b->d / g;
  return (newR(
   a->n * t + b->n * (a->d / g)
  ,a->d * t
  ));
}
#endif

static void
addToR(
  rat_t *a
 ,const rat_t *b
){
  long g;
  long t;

  if (!(g = gcdI(a->d, b->d))) {
    a->n = a->d = 0;
    return;
  }
  t = b->d / g;
  a->n = a->n * t + b->n * (a->d / g);
  a->d = a->d * t;
}

/* a/b - c/d = (ad - cb) / bd */
#if 0
static rat_t *
subR(
  const rat_t *a
 ,const rat_t *b
){
  long g;
  long t;

  if (!(g = gcdI(a->d, b->d)))
    return (0);
  t = a->d / g;
  return (newR(
   a->n * (b->d / g) - b->n * t
  ,b->d * t
  ));
}
#endif

#if 0
static void
subFromR(
  rat_t *a
 ,const rat_t *b
){
  long g;
  long t;

  if (!(g = gcdI(a->d, b->d))) {
    a->n = a->d = 0;
    return;
  }
  t = a->d / g;
  a->n = a->n * (b->d / g) - b->n * t;
  a->d = b->d * t;
}
#endif

/* a/b . c/d = ac / bd */
#if 0
static rat_t *
mulR(
  const rat_t *a
 ,const rat_t *b
){
  long g1;
  long g2;
  long n;
  long d;

  if (!(g1 = gcdI(a->n, b->d))
   || !(g2 = gcdI(b->n, a->d)))
    return (0);
  n = (a->n / g1) * (b->n / g2);
  d = (a->d / g2) * (b->d / g1);
  g1 = gcdI(n, d);
  return (newR(
   n / g1
  ,d / g1
  ));
}
#endif

static void
mulByR(
  rat_t *a
 ,const rat_t *b
){
  long g1;
  long g2;
  long n;
  long d;

  if (!(g1 = gcdI(a->n, b->d))
   || !(g2 = gcdI(b->n, a->d))) {
    a->n = a->d = 0;
    return;
  }
  n = (a->n / g1) * (b->n / g2);
  d = (a->d / g2) * (b->d / g1);
  g1 = gcdI(n, d);
  a->n = n / g1;
  a->d = d / g1;
}

/* neg a/b = -1/b */
#if 0
static rat_t *
negR(
  const rat_t *a
){
  if (a->d < 0)
    return (newR(
     a->n
    ,-a->d
    ));
  else
    return (newR(
     -a->n
    ,a->d
    ));
}
#endif

static void
negOfR(
  rat_t *a
){
  if (a->d < 0)
    a->d = -a->d;
  else
    a->n = -a->n;
}

/* reciprocal a/b = b/a */
#if 0
static rat_t *
rcpR(
  const rat_t *a
){
  if (!a->n)
    return (0);
  return (newR(
   a->d
  ,a->n
  ));
}
#endif

static void
rcpOfR(
  rat_t *a
){
  long t;

  if (!(t = a->n)) {
    a->n = a->d = 0;
    return;
  }
  a->n = a->d;
  a->d = t;
}

/*************************************************************************/

/* constant stream context */
struct conS_ {
  chan_t *o; /* output channel */
  rat_t c;   /* constant */
};

/* constant stream thread */
static void *
conS_(
void *v
#define V ((struct conS_ *)v)
){
  rat_t *o;

  while ((o = dupR(&V->c))) {
    if (chanOp(0, V->o, (void **)&o, chanOpPut) != chanOsPut) {
      free(o);
      break;
    }
  }
  chanShut(V->o);
  chanClose(V->o);
  free(v);
  return (0);
}
#undef V

/* launch constant stream thread */
static int
conS(
  chan_t *o
 ,const rat_t *c
){
  struct conS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->o = chanOpen(o);
  cpyR(&x->c, c);
  if (pthread_create(&pt, 0, conS_, x)) {
    chanClose(x->o);
    free(x);
oor:
    fprintf(stderr, "conS OoR\n");
    chanShut(o);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = Ft */
struct multS_ {
  chan_t *p;
  chan_t *f;
  rat_t t;
};

static void *
multS_(
void *v
#define V ((struct multS_ *)v)
){
  rat_t *f;
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
  chanShut(V->p);
  chanClose(V->p);
  chanShut(V->f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
multS(
  chan_t *p
 ,chan_t *f
 ,const rat_t *t
){
  struct multS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  cpyR(&x->t, t);
  if (pthread_create(&pt, 0, multS_, x)) {
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "multS OoR\n");
    chanShut(p);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream S = F + G */
struct addS_ {
  chan_t *s;
  chan_t *f;
  chan_t *g;
};

static void *
addS_(
void *v
#define V ((struct addS_ *)v)
){
  rat_t *f;
  rat_t *g;
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
  chanShut(V->s);
  chanClose(V->s);
  chanShut(V->f);
  chanClose(V->f);
  chanShut(V->g);
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
  struct addS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->s = chanOpen(s);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, addS_, x)) {
    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->s);
    free(x);
oor:
    fprintf(stderr, "addS OoR\n");
    chanShut(s);
    chanShut(f);
    chanShut(g);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = x^n F */
struct xnS_ {
  chan_t *p;
  chan_t *f;
  unsigned long n;
};

static void *
xnS_(
void *v
#define V ((struct xnS_ *)v)
){
  rat_t *f;
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
  chanShut(V->p);
  chanClose(V->p);
  chanShut(V->f);
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
  struct xnS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->n = n;
  if (pthread_create(&pt, 0, xnS_, x)) {
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "xnS OoR\n");
    chanShut(p);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = FG */
struct mulS_ {
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
mulS_(
void *v
#define V ((struct mulS_ *)v)
){
  enum ch {
   P = 0, F, G, F0, F1, G0, G1, Fg, Gf, FG, xFG, chCnt
  };
  rat_t *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F, G */
  chanArr_t pa1[chCnt]; /* put P */
  chanArr_t pa2[chCnt]; /* put F0, F1, G0, G1 */
  chanArr_t ga2[chCnt]; /* get Fg, Gf, xFG */
  rat_t f;
  rat_t g;
  unsigned int i;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  memset(ga2, 0, sizeof (ga2));
  ga1[P].c = V->p;
  ga1[F].c = V->f;
  ga1[G].c = V->g;
  for (i = 0; i < chCnt; ++i) {
    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, (chanSd_t)free))) {
      fprintf(stderr, "mulS_ OoR\n");
      goto exit;
    }
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));
  memcpy(pa2, ga1, sizeof (pa2));
  memcpy(ga2, ga1, sizeof (ga2));

  ga1[F].o = ga1[G].o = chanOpGet;
  pa1[P].o = chanOpPut;
  if ((i = chanAll(0, sizeof (ga1) / sizeof (ga1[0]), ga1)) != chanAlOp)
    goto exit;
  cpyR(&f, r[F]);
  cpyR(&g, r[G]);
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
  if (multS(ga2[Fg].c, pa2[F0].c, &g)
   || multS(ga2[Gf].c, pa2[G0].c, &f)
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
  for (i = 0; i < chCnt; ++i) {
    chanShut(ga1[i].c);
    chanClose(ga1[i].c);
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
  struct mulS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, mulS_, x)) {
    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "mulS OoR\n");
    chanShut(p);
    chanShut(f);
    chanShut(g);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = dF/dx */
struct dffS_ {
  chan_t *p;
  chan_t *f;
};

static void *
dffS_(
void *v
#define V ((struct dffS_ *)v)
){
  rat_t *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  rat_t n;

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
  setR(&n, 0, 1);
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    if (!++n.n) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
  }
exit:
  chanShut(V->p);
  chanClose(V->p);
  chanShut(V->f);
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
  struct dffS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, dffS_, x)) {
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "dffS OoR\n");
    chanShut(p);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream P = integrate(0,inf)F(x) + c */
struct ntgS_ {
  chan_t *p;
  chan_t *f;
  rat_t c;
};

static void *
ntgS_(
void *v
#define V ((struct ntgS_ *)v)
){
  rat_t *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  rat_t n;

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

  if (!(f = dupR(&V->c))
   || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
    free(f);
    goto exit;
  }
  setR(&n, 1, 0);
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    if (!++n.d) {
      free(f);
      break;
    }
    mulByR(f, &n);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
  }
exit:
  chanShut(V->p);
  chanClose(V->p);
  chanShut(V->f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
ntgS(
  chan_t *p
 ,chan_t *f
 ,const rat_t *c
){
  struct ntgS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  cpyR(&x->c, c);
  if (pthread_create(&pt, 0, ntgS_, x)) {
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "ntgS OoR\n");
    chanShut(p);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream S = F(G) */
struct sbtS_ {
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
sbtS_(
void *v
#define V ((struct sbtS_ *)v)
){
  enum ch {
   S = 0, F, G, G0, G1, FG, chCnt
  };
  rat_t *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then G */
  chanArr_t pa1[chCnt]; /* put S then G0, G1 */
  unsigned int i;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  ga1[S].c = V->s;
  ga1[F].c = V->f;
  ga1[G].c = V->g;
  for (i = 0; i < chCnt; ++i) {
    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, (chanSd_t)free))) {
      fprintf(stderr, "sbtS_ OoR\n");
      goto exit;
    }
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));

  ga1[F].o = chanOpGet;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet)
    goto exit;
  r[S] = r[F];
  pa1[S].o = chanOpPut;
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
  pa1[S].o = chanOpSht;
  if (sbtS(ga1[FG].c, pa1[F].c, pa1[G1].c)
   || mulS(ga1[S].c, pa1[G0].c, pa1[FG].c))
    goto exit;
  ga1[F].o = chanOpSht;
  ga1[G].o = chanOpGet;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != G + 1 || ga1[G].s != chanOsGet)
    goto exit;
  r[G1] = r[G];
  pa1[G0].o = pa1[G1].o = chanOpPut;
  while (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) == G + 1 && ga1[G].s == chanOsGet) {
    if (!(r[G0] = dupR(r[G]))
     || chanAll(0, sizeof (pa1) / sizeof (pa1[0]), pa1) != chanAlOp) {
      free(r[G0]);
      free(r[G]);
      break;
    }
    r[G1] = r[G];
  }
  free(r[G1]);
exit:
  for (i = 0; i < chCnt; ++i) {
    chanShut(ga1[i].c);
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
  struct sbtS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->s = chanOpen(s);
  x->f = chanOpen(f);
  x->g = chanOpen(g);
  if (pthread_create(&pt, 0, sbtS_, x)) {
    chanClose(x->g);
    chanClose(x->f);
    chanClose(x->s);
    free(x);
oor:
    fprintf(stderr, "sbtS OoR\n");
    chanShut(s);
    chanShut(f);
    chanShut(g);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream E = exp(F) */
struct expS_ {
  chan_t *e;
  chan_t *f;
};

static void *
expS_(
void *v
#define V ((struct expS_ *)v)
){
  enum ch {
   X0 = 0, F, D, P, X, X1, chCnt
  };
  rat_t *r[chCnt];
  chanArr_t ga1[chCnt]; /* get X */
  chanArr_t pa1[chCnt]; /* put X0, X1 */
  unsigned int i;
  rat_t c;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  ga1[F].c = V->f;
  ga1[X0].c = V->e;
  for (i = 0; i < chCnt; ++i) {
    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, (chanSd_t)free))) {
      fprintf(stderr, "expS_ OoR\n");
      goto exit;
    }
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));

  setR(&c, 1, 1);
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
  for (i = 0; i < chCnt; ++i) {
    chanShut(ga1[i].c);
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
  struct expS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->e = chanOpen(e);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, expS_, x)) {
    chanClose(x->f);
    chanClose(x->e);
    free(x);
oor:
    fprintf(stderr, "expS OoR\n");
    chanShut(e);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream R = 1/F */
struct rcpS_ {
  chan_t *r;
  chan_t *f;
};

static void *
rcpS_(
void *v
#define V ((struct rcpS_ *)v)
){
  enum ch {
   R = 0, F, M, RM, RT, chCnt
  };
  rat_t *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then RT */
  chanArr_t pa1[chCnt]; /* put R */
  chanArr_t pa2[chCnt]; /* put RM */
  unsigned int i;
  rat_t n;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  ga1[R].c = V->r;
  ga1[F].c = V->f;
  for (i = 0; i < chCnt; ++i) {
    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, (chanSd_t)free))) {
      fprintf(stderr, "rcpS_ OoR\n");
      goto exit;
    }
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
  cpyR(&n, r[F]);
  negOfR(&n);
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
   || multS(ga1[RT].c, pa1[M].c, &n))
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
  for (i = 0; i < chCnt; ++i) {
    chanShut(ga1[i].c);
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
  struct rcpS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->r = chanOpen(r);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, rcpS_, x)) {
    chanClose(x->f);
    chanClose(x->r);
    free(x);
oor:
    fprintf(stderr, "rcpS OoR\n");
    chanShut(r);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream R = reversion: functional inverse of F, an R such that F(R(x)) = x */
struct revS_ {
  chan_t *r;
  chan_t *f;
};

static void *
revS_(
void *v
#define V ((struct revS_ *)v)
){
  enum ch {
   R0 = 0, F, R1, FR1, RB2, RB0, RB1, RB2FR1, R, chCnt
  };
  rat_t *r[chCnt];
  chanArr_t ga1[chCnt]; /* get F then R */
  chanArr_t pa1[chCnt]; /* put R0 R1 */
  chanArr_t pa2[chCnt]; /* put RB0 RB1 */
  unsigned int i;
  rat_t n;

  memset(ga1, 0, sizeof (ga1));
  memset(pa1, 0, sizeof (pa1));
  memset(pa2, 0, sizeof (pa2));
  ga1[R0].c = V->r;
  ga1[F].c = V->f;
  for (i = 0; i < chCnt; ++i) {
    if (!ga1[i].c && !(ga1[i].c = chanCreate(0, 0, (chanSd_t)free))) {
      fprintf(stderr, "revS_ OoR\n");
      goto exit;
    }
    ga1[i].v = (void **)&r[i];
    ga1[i].o = chanOpSht;
  }
  memcpy(pa1, ga1, sizeof (pa1));
  memcpy(pa2, ga1, sizeof (pa2));

  ga1[F].o = chanOpGet;
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet)
    goto exit;
  r[R0] = r[F];
  if (chanOne(0, sizeof (ga1) / sizeof (ga1[0]), ga1) != F + 1 || ga1[F].s != chanOsGet) {
    free(r[R0]);
    goto exit;
  }
  rcpOfR(r[F]);
  cpyR(&n, r[F]);
  negOfR(&n);
  if (sbtS(ga1[FR1].c, pa1[F].c, pa1[R1].c)
   || mulS(ga1[RB2].c, pa2[RB0].c, pa2[RB1].c)
   || mulS(ga1[RB2FR1].c, pa2[RB2].c, pa1[FR1].c)
   || multS(ga1[R].c, pa1[RB2FR1].c, &n)) {
    free(r[R0]);
    free(r[F]);
    goto exit;
  }
  ga1[F].o = chanOpSht;
  ga1[R].o = chanOpGet;
  pa1[R0].o = pa1[R1].o = chanOpPut;
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
  for (i = 0; i < chCnt; ++i) {
    chanShut(ga1[i].c);
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
  struct revS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->r = chanOpen(r);
  x->f = chanOpen(f);
  if (pthread_create(&pt, 0, revS_, x)) {
    chanClose(x->f);
    chanClose(x->r);
    free(x);
oor:
    fprintf(stderr, "revS OoR\n");
    chanShut(r);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* stream monomial substitution P = F(cx^n) */
struct msbtS_ {
  chan_t *p;
  chan_t *f;
  rat_t c;
  unsigned long n;
};

static void *
msbtS_(
void *v
#define V ((struct msbtS_ *)v)
){
  rat_t *f;
  chanArr_t ga[2];
  chanArr_t pa[2];
  rat_t c;
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

  setR(&c, 1, 1);
  while (chanOne(0, sizeof (ga) / sizeof (ga[0]), ga) == 2 && ga[1].s == chanOsGet) {
    mulByR(f, &c);
    if (chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
      free(f);
      break;
    }
    mulByR(&c, &V->c);
    for (i = 1; i < V->n; ++i)
      if (!(f = newR(0, 1))
       || chanOne(0, sizeof (pa) / sizeof (pa[0]), pa) != 1 || pa[0].s != chanOsPut) {
        free(f);
        goto exit;
      }
  }
exit:
  chanShut(V->p);
  chanClose(V->p);
  chanShut(V->f);
  chanClose(V->f);
  free(v);
  return (0);
}
#undef V

static int
msbtS(
  chan_t *p
 ,chan_t *f
 ,const rat_t *c
 ,unsigned long n
){
  struct msbtS_ *x;
  pthread_t pt;

  if (!(x = malloc(sizeof (*x))))
    goto oor;
  x->p = chanOpen(p);
  x->f = chanOpen(f);
  cpyR(&x->c, c);
  x->n = n;
  if (pthread_create(&pt, 0, msbtS_, x)) {
    chanClose(x->f);
    chanClose(x->p);
    free(x);
oor:
    fprintf(stderr, "msbtS OoR\n");
    chanShut(p);
    chanShut(f);
    return (1);
  }
  pthread_detach(pt);
  return (0);
}

/*************************************************************************/

/* print n rationals from a stream then shut it down */
static void
printS(
  chan_t *c
 ,unsigned int n
){
  rat_t *o;

  for (; n; --n) {
    if (chanOp(0, c, (void **)&o, chanOpGet) != chanOsGet)
      break;
    printf(" %ld/%ld", o->n, o->d),fflush(stdout);
    free(o);
  }
  putchar('\n'),fflush(stdout);
  chanShut(c);
}

int
main(
 int argc
,char *argv[]
){
  chan_t *c1;
  chan_t *c2;
  chan_t *c3;
  chan_t *c4;
  rat_t r1;
  rat_t r2;
  rat_t r3;
  unsigned int count;

  if (argc != 2)
    count = 12;
  else if (!(count = atoi(argv[1]))) {
    fprintf(stderr, "Usage %s: count\n", argv[0]);
    return (-1);
  }
  chanInit(realloc, free);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1))
    goto exit;
  printf("conS:"),fflush(stdout);
  printS(c1, count);
  chanClose(c1);

  setR(&r1, 1, 1);
  setR(&r2, -1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || multS(c2, c1, &r2))
    goto exit;
  printf("multS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, (chanSd_t)free))
   || addS(c3, c1, c2))
    goto exit;
  printf("addS:"),fflush(stdout);
  printS(c3, count);
  chanClose(c1);
  chanClose(c2);
  chanClose(c3);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || xnS(c2, c1, 1))
    goto exit;
  printf("xnS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, (chanSd_t)free))
   || mulS(c3, c1, c2))
    goto exit;
  printf("mulS:"),fflush(stdout);
  printS(c3, count);
  chanClose(c1);
  chanClose(c2);
  chanClose(c3);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || dffS(c2, c1))
    goto exit;
  printf("dffS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  setR(&r2, 0, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || ntgS(c2, c1, &r2))
    goto exit;
  printf("ntgS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c2, &r1)
   || !(c3 = chanCreate(0, 0, (chanSd_t)free))
   || sbtS(c3, c1, c2))
    goto exit;
  printf("sbtS:"),fflush(stdout);
  printS(c3, count);
  chanClose(c1);
  chanClose(c2);
  chanClose(c3);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || expS(c2, c1))
    goto exit;
  printf("expS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || rcpS(c2, c1))
    goto exit;
  printf("rcpS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || revS(c2, c1))
    goto exit;
  printf("revS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  setR(&r2, -1, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || msbtS(c2, c1, &r2, 2))
    goto exit;
  printf("msbtS:"),fflush(stdout);
  printS(c2, count);
  chanClose(c1);
  chanClose(c2);

  setR(&r1, 1, 1);
  setR(&r2, -1, 1);
  setR(&r3, 0, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || msbtS(c2, c1, &r2, 2)
   || !(c3 = chanCreate(0, 0, (chanSd_t)free))
   || ntgS(c3, c2, &r3))
    goto exit;
  printf("ntg-msbt:"),fflush(stdout);
  printS(c3, count);
  chanClose(c1);
  chanClose(c2);
  chanClose(c3);

  setR(&r1, 1, 1);
  setR(&r2, -1, 1);
  setR(&r3, 0, 1);
  if (!(c1 = chanCreate(0, 0, (chanSd_t)free))
   || conS(c1, &r1)
   || !(c2 = chanCreate(0, 0, (chanSd_t)free))
   || msbtS(c2, c1, &r2, 2)
   || !(c3 = chanCreate(0, 0, (chanSd_t)free))
   || ntgS(c3, c2, &r3)
   || !(c4 = chanCreate(0, 0, (chanSd_t)free))
   || revS(c4, c3))
    goto exit;
  printf("tanS:"),fflush(stdout);
  printS(c4, count);
  chanClose(c1);
  chanClose(c2);
  chanClose(c3);
  chanClose(c4);

  return (0);
exit:
  return (-1);
}
