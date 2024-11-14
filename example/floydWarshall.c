/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2024 G. David Butler <gdb@dbSystems.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#ifdef FWSQLITE
#include "sqlite3.h"
#endif
#include "floydWarshall.h"

void
fwFree(
  struct fw *v
){
  if (v) {
#ifdef FWEQL
    fwNxt_t i;
    fwNxt_t j;

    for (i = 0; i < v->d; ++i)
      for (j = 0; j < v->d; ++j)
        if ((v->nxt + v->d * i + j)->x)
          free((v->nxt + v->d * i + j)->x);
#endif /* FWEQL */
    free(v->nxt);
    free(v->cst);
    free(v);
  }
}

struct fw *
fwAlloc(
  unsigned long d
#ifdef FWBLK
,fwNxt_t b /* dependent on target processor cache size */
#endif /* FWBLK */
){
  struct fw *fw;
  fwNxt_t i;
  fwNxt_t j;

#ifdef FWBLK
  if (d <= b)
    b = d;
  else if ((i = d % b))
    d += b - i;
#endif /* FWBLK */
  if (d > 1LU << (sizeof (fwNxt_t) * 8 - 1)) /* problem too big for next element size */
    return (0);
  if (!(fw = calloc(1, sizeof (*fw)))
   || !(fw->cst = malloc(d * d * sizeof (*fw->cst)))
   || !(fw->nxt = calloc(d * d, sizeof (*fw->nxt)))) {
    if (fw) {
      free(fw->cst);
      free(fw);
    }
    return (0);
  }
  fw->d = d;
  for (i = 0; i < d; ++i)
    for (j = 0; j < d; ++j)
      *(fw->cst + d * i + j) = (1LU << (sizeof (fwCst_t) * 8 - 1)) - 1;
#ifdef FWBLK
  fw->b = b;
#endif /* FWBLK */
  return (fw);
}

struct fw *
fwDup(
  const struct fw *v
){
  struct fw *fw;
  fwNxt_t d;
  fwNxt_t i;
  fwNxt_t j;
#ifdef FWEQL
  fwNxt_t l;
  fwNxt_t k;
#endif

  if (!v)
    return (0);
  d = v->d;
  if (!(fw = calloc(1, sizeof (*fw)))
   || !(fw->cst = malloc(d * d * sizeof (*fw->cst)))
   || !(fw->nxt = calloc(d * d, sizeof (*fw->nxt)))) {
    if (fw) {
      free(fw->cst);
      free(fw);
    }
    return (0);
  }
  fw->d = d;
  for (i = 0; i < d; ++i) {
    for (j = 0; j < d; ++j) {
      *(fw->cst + d * i + j) = *(v->cst + d * i + j);
#ifdef FWEQL
      l = (fw->nxt + d * i + j)->l = (v->nxt + d * i + j)->l;
      if (!(v->nxt + d * i + j)->x)
        continue;
      if (!((fw->nxt + d * i + j)->x = malloc(l * sizeof (*fw->nxt->x)))) {
        fwFree(fw);
        return (0);
      } else {
        for (k = 0; k < l; ++k)
          *((fw->nxt + d * i + j)->x + k) = *((v->nxt + d * i + j)->x + k);
      }
#else
      *(fw->nxt + d * i + j) = *(v->nxt + d * i + j);
#endif
    }
  }
#ifdef FWBLK
  fw->b = v->b;
#endif /* FWBLK */
  return (fw);
}

int
fwCmp(
  const struct fw *v1
 ,const struct fw *v2
){
  fwNxt_t d;
  fwNxt_t i;
  fwNxt_t j;
#ifdef FWEQL
  fwNxt_t l;
  fwNxt_t k;
#endif

  if (!v1 && !v2)
    return (0);
  if (!v1 || !v2)
    return (1);
  if (v1->d != v2->d)
    return (1);
  d = v1->d;
  for (i = 0; i < d; ++i) {
    for (j = 0; j < d; ++j) {
      if (*(v1->cst + d * i + j) != *(v2->cst + d * i + j)
#ifdef FWEQL
       || (v1->nxt + d * i + j)->l != (v2->nxt + d * i + j)->l
#else
       || *(v1->nxt + d * i + j) != *(v2->nxt + d * i + j)
#endif
      )
        return (1);
#ifdef FWEQL
      if (!(v1->nxt + d * i + j)->x && !(v2->nxt + d * i + j)->x)
        continue;
      if (!(v1->nxt + d * i + j)->x || !(v2->nxt + d * i + j)->x)
        return (1);
      l = (v1->nxt + d * i + j)->l;
      for (k = 0; k < l; ++k)
        if (*((v1->nxt + d * i + j)->x + k) != *((v2->nxt + d * i + j)->x + k))
          return (1);
#endif
    }
  }
  return (0);
}

#ifdef FWEQL
static int
fwNxtAdd(
  struct fwNxt *t
 ,struct fwNxt *f
){
  void *tv;
  fwNxt_t i;
  fwNxt_t j;
  fwNxt_t l;

  if (!t->x && !f->x) {
    if (t->l != f->l) {
      if (!(tv = realloc(t->x, (1 + 1) * sizeof (*t->x))))
        return (1);
      t->x = tv;
      *(t->x + 0) = t->l;
      *(t->x + 1) = f->l;
      t->l = 1 + 1;
    }
  } else if (!t->x && f->x) {
    for (i = 0; i < f->l; ++i)
      if (t->l == *(f->x + i))
        break;
    if (i < f->l) {
      if (!(tv = realloc(t->x, f->l * sizeof (*t->x))))
        return (1);
      t->x = tv;
      for (l = f->l; l--; )
        *(t->x + l) = *(f->x + l);
      t->l = f->l;
    } else {
      if (!(tv = realloc(t->x, (1 + f->l) * sizeof (*t->x))))
        return (1);
      t->x = tv;
      *(t->x + 0) = t->l;
      for (l = f->l; l--; )
        *(t->x + 1 + l) = *(f->x + l);
      t->l = f->l + 1;
    }
  } else if (t->x && !f->x) {
    for (i = 0; i < t->l; ++i)
      if (*(t->x + i) == f->l)
        break;
    if (i == t->l) {
      if (!(tv = realloc(t->x, (t->l + 1) * sizeof (*t->x))))
        return (1);
      t->x = tv;
      *(t->x + t->l++) = f->l;
    }
  } else {
    for (i = 0; i < f->l; ++i) {
      for (j = 0; j < t->l; ++j)
        if (*(t->x + j) == *(f->x + i))
          break;
      if (j == t->l) {
        if (!(tv = realloc(t->x, (t->l + 1) * sizeof (*t->x))))
          return (1);
        t->x = tv;
        *(t->x + t->l++) = *(f->x + i);
      }
    }
  }
  return (0);
}
#endif /* FWEQL */

static
#ifdef __STDC__
# ifdef __STDC_VERSION__
#  if __STDC_VERSION__ >= 199901L
inline
#  endif
# endif
#endif
int
fwProcess0(
  fwCst_t *cc
 ,fwCst_t *ac
 ,fwCst_t *bc
#ifdef FWEQL
 ,struct fwNxt *cn
 ,struct fwNxt *an
#else /* FWEQL */
 ,fwNxt_t *cn
 ,fwNxt_t *an
#endif /* FWEQL */
 ,fwNxt_t d
 ,fwNxt_t b
){
#ifdef FWEQL
  fwNxt_t l;
#endif /* FWEQL */
  fwNxt_t k;
  fwNxt_t i;
  fwNxt_t j;
  fwCst_t c;

  for (k = 0; k < b; ++k)
    for (i = 0; i < b; ++i)
      for (j = 0; j < b; ++j)
        if (*(cc + d * i + j) > *(ac + d * i + k)
         && *(cc + d * i + j) > *(bc + d * k + j)) {
          if (*(cc + d * i + j) > (c = *(ac + d * i + k) + *(bc + d * k + j))) {
            if ((*(cc + d * i + j) = c) < 0 && i == j)
              return (1);
#ifdef FWEQL
            if ((cn + d * i + j)->x)
              free((cn + d * i + j)->x);
            if ((an + d * i + k)->x) {
              if (!((cn + d * i + j)->x = malloc((an + d * i + k)->l * sizeof (*cn->x))))
                return (1);
              for (l = (an + d * i + k)->l; l--; )
                *((cn + d * i + j)->x + l) = *((an + d * i + k)->x + l);
            } else
              (cn + d * i + j)->x = 0;
            (cn + d * i + j)->l = (an + d * i + k)->l;
#else /* FWEQL */
            *(cn + d * i + j) = *(an + d * i + k);
#endif /* FWEQL */
          }
#ifdef FWEQL
          else if (*(cc + d * i + j) == c && fwNxtAdd(cn + d * i + j, an + d * i + k))
            return (1);
#endif /* FWEQL */
        }
  return (0);
}

#ifdef FWBLK
#include "chan.h"

struct fwProcess {
  fwCst_t *cst;
#ifdef FWEQL
  struct fwNxt *nxt;
#else /* FWEQL */
  fwNxt_t *nxt;
#endif /* FWEQL */
  void (*f)(struct fwProcess *);
  int r;
  fwNxt_t d;
  fwNxt_t b;
  fwNxt_t k;
  fwNxt_t p;
  fwNxt_t pb;
};

static void
fwProcess1(
  struct fwProcess *v
){
  fwNxt_t p;

  for (p = v->p; p < v->pb; p += v->b) {
    if (p == v->k)
      continue;
    if ((v->r =
     fwProcess0(v->cst + v->d * v->k + p
               ,v->cst + v->d * v->k + v->k
               ,v->cst + v->d * v->k + p
               ,v->nxt + v->d * v->k + p
               ,v->nxt + v->d * v->k + v->k
               ,v->d
               ,v->b)
    ))
      return;
  }
}

static void
fwProcess2(
  struct fwProcess *v
){
  fwNxt_t p;
  fwNxt_t j;

  for (p = v->p; p < v->pb; p += v->b) {
    if (p == v->k)
      continue;
    if ((v->r =
     fwProcess0(v->cst + v->d * p + v->k
               ,v->cst + v->d * p + v->k
               ,v->cst + v->d * v->k + v->k
               ,v->nxt + v->d * p + v->k
               ,v->nxt + v->d * p + v->k
               ,v->d
               ,v->b)
    ))
      return;
    for (j = 0; j < v->d; j += v->b) {
      if (j == v->k)
        continue;
      if ((v->r =
       fwProcess0(v->cst + v->d * p + j
                 ,v->cst + v->d * p + v->k
                 ,v->cst + v->d * v->k + j
                 ,v->nxt + v->d * p + j
                 ,v->nxt + v->d * p + v->k
                 ,v->d
                 ,v->b)
      ))
        return;
    }
  }
}

struct fwThread {
  chan_t *snk;
  chan_t *src;
};

static void *
fwThread(
  void *v
){
#define V ((struct fwThread *)v)
  struct fwProcess *s;

  while(chanOp(0, V->snk, (void **)&s, chanOpGet) == chanOsGet) {
    s->f(s);
    chanOp(0, V->src, (void **)&s, chanOpPut);
  }
  chanClose(V->src);
  chanClose(V->snk);
  free(v);
  return (0);
#undef V
}

int
fwProcess(
 struct fw *v
,fwNxt_t p
){
  struct fwProcess *fwp;
  void *s;
  struct fwThread fwt;
  fwNxt_t k;
  fwNxt_t c;
  fwNxt_t i;
  int r;

  if (!v)
    return (1);
  if (v->d <= v->b)
    return (fwProcess0(v->cst, v->cst, v->cst, v->nxt, v->nxt, v->d, v->d));
  r = 1;
  if ((c = v->d / v->b) < p)
    p = c;
  c = v->b * (c / p);
  if (p > 1) {
    if (!(fwt.snk = chanCreate(0, 0, 0, 0)) || !(fwt.src = chanCreate(0, 0, 0, 0))) {
      chanClose(fwt.snk);
      return (r);
    }
  } else
    fwt.snk = fwt.src = 0;
  if (!(fwp = malloc(p * sizeof (*fwp))))
    goto exit;
  for (i = 0; i < p - 1; ++i) {
    struct fwThread *c;
    pthread_t t;

    if (!(c = malloc(sizeof (*c)))
     || !(c->snk = chanOpen(fwt.snk))
     || !(c->src = chanOpen(fwt.src))
     || pthread_create(&t, 0, fwThread, c)) {
      if (c) {
        chanClose(c->src);
        chanClose(c->snk);
        free(c);
      }
      goto exit;
    }
    pthread_detach(t);
  }
  for (i = 0; i < p; ++i) {
    (fwp + i)->cst = v->cst;
    (fwp + i)->nxt = v->nxt;
    (fwp + i)->r = 0;
    (fwp + i)->d = v->d;
    (fwp + i)->b = v->b;
    (fwp + i)->p = i * c;
    (fwp + i)->pb = (fwp + i)->p + c;
  }
  (fwp + (i - 1))->pb = v->d;
  for (k = 0; k < v->d; k += v->b) {
    if (fwProcess0(v->cst + v->d * k + k
              ,v->cst + v->d * k + k
              ,v->cst + v->d * k + k
              ,v->nxt + v->d * k + k
              ,v->nxt + v->d * k + k
              ,v->d
              ,v->b))
      goto exit;
    for (i = 0; i < p - 1; ++i) {
      (fwp + i)->k = k;
      (fwp + i)->f = fwProcess1;
      s = fwp + i;
      chanOp(0, fwt.snk, (void **)&s, chanOpPut);
    }
    (fwp + i)->k = k;
    fwProcess1(fwp + i);
    for (i = 0; i < p - 1; ++i)
      chanOp(0, fwt.src, (void **)&s, chanOpGet);
    for (i = 0; i < p; ++i)
      if ((fwp + i)->r)
        goto exit;
    for (i = 0; i < p - 1; ++i) {
      (fwp + i)->f = fwProcess2;
      s = fwp + i;
      chanOp(0, fwt.snk, (void **)&s, chanOpPut);
    }
    fwProcess2(fwp + i);
    for (i = 0; i < p - 1; ++i)
      chanOp(0, fwt.src, (void **)&s, chanOpGet);
    for (i = 0; i < p; ++i)
      if ((fwp + i)->r)
        goto exit;
  }
  r = 0;
exit:
  if (p > 1) {
    chanShut(fwt.snk);
    chanShut(fwt.src);
    chanClose(fwt.snk);
    chanClose(fwt.src);
  }
  free(fwp);
  return (r);
}

#else /* FWBLK */

int
fwProcess(
  struct fw *v
){
  if (!v)
    return (1);
  return (fwProcess0(v->cst, v->cst, v->cst, v->nxt, v->nxt, v->d, v->d));
}
#endif /* FWBLK */

#ifdef FWSQLITE

struct fwcTab {
  sqlite3_vtab t;
  struct fw *p;
};

static int
fwcCon(
  sqlite3 *d
 ,void *u1
 ,int u2
 ,const char *const *u3
 ,sqlite3_vtab **v
 ,char **u4
){
  struct fwcTab *xt;
  int i;

  if ((i = sqlite3_declare_vtab(d, "CREATE TABLE Fwc(f INTEGER, t INTEGER, c NUMERIC, p HIDDEN)")))
    return (i);
  if (!(xt = sqlite3_malloc(sizeof (*xt))))
    return (SQLITE_NOMEM);
  xt->p = 0;
  *v = &xt->t;
  sqlite3_vtab_config(d, SQLITE_VTAB_DIRECTONLY);
  return (SQLITE_OK);
  (void)u1;
  (void)u2;
  (void)u3;
  (void)u4;
}

static int
fwcDis(
  sqlite3_vtab *v
){
  sqlite3_free(v);
  return (SQLITE_OK);
}

struct fwcCsr {
  sqlite3_vtab_cursor c;
  struct fw *p;
  fwNxt_t f;
  fwNxt_t t;
  fwNxt_t fs;
  fwNxt_t fe;
  fwNxt_t ts;
  fwNxt_t te;
};

static int
fwcOpn(
  sqlite3_vtab *u1
 ,sqlite3_vtab_cursor **c
){
  struct fwcCsr *xc;

  if (!(xc = sqlite3_malloc(sizeof (*xc))))
    return (SQLITE_NOMEM);
  xc->p = 0;
  xc->f = xc->t = xc->fs = xc->fe = xc->ts = xc->te = 0;
  *c = &xc->c;
  return (SQLITE_OK);
  (void)u1;
}

static int
fwcCls(
  sqlite3_vtab_cursor *c
){
  sqlite3_free(c);
  return (SQLITE_OK);
}

static int
fwcBst(
  sqlite3_vtab *u1,
  sqlite3_index_info *i
){
  int n;
  int f;
  int t;
  int p;

  f = t = p = -1;
  for (n = 0; n < i->nConstraint; ++n) {
    if (!(i->aConstraint + n)->usable)
      continue;
    switch ((i->aConstraint + n)->iColumn) {
    case 0: /* f */
      if ((i->aConstraint + n)->op != SQLITE_INDEX_CONSTRAINT_EQ)
        continue;
      f = n;
      break;
    case 1: /* t */
      if ((i->aConstraint + n)->op != SQLITE_INDEX_CONSTRAINT_EQ)
        continue;
      t = n;
      break;
#if 0
    case 2: /* c */
      break;
#endif
    case 3: /* p */
      p = n;
      break;
    default:
      break;
    }
  }
  n = 0;
  if (p >= 0) {
    (i->aConstraintUsage + p)->argvIndex = ++n;
    (i->aConstraintUsage + p)->omit = 1;
    i->idxNum |= 1;
    if (f >= 0) {
      (i->aConstraintUsage + f)->argvIndex = ++n;
      (i->aConstraintUsage + f)->omit = 1;
      i->idxNum |= 2;
    }
    if (t >= 0) {
      (i->aConstraintUsage + t)->argvIndex = ++n;
      (i->aConstraintUsage + t)->omit = 1;
      i->idxNum |= 4;
    }
  }
  if (i->idxNum & 1) {
    if (i->idxNum & 2 && i->idxNum & 4) {
      i->estimatedCost = 0.1;
      i->estimatedRows = 1;
    } else if (i->idxNum & 2 || i->idxNum & 4) {
      i->estimatedCost = 10.0;
      i->estimatedRows = 100;
    } else {
      i->estimatedCost = 100.0;
      i->estimatedRows = 100000;
    }
  }
  if (!i->idxNum)
    return (SQLITE_ERROR);
  if (i->nOrderBy == 2
   && (i->aOrderBy + 0)->iColumn == 0 && !(i->aOrderBy + 0)->desc
   && (i->aOrderBy + 1)->iColumn == 1 && !(i->aOrderBy + 1)->desc)
    i->orderByConsumed = 1;
  else if (i->nOrderBy == 1
   && (i->aOrderBy + 0)->iColumn == 0 && !(i->aOrderBy + 0)->desc)
    i->orderByConsumed = 1;
  return (SQLITE_OK);
  (void)u1;
}

static int
fwcFlt(
  sqlite3_vtab_cursor *c
 ,int x
 ,const char *u1
 ,int n
 ,sqlite3_value **a
){
  sqlite3_int64 i;

  if (n && x & 1) {
    ((struct fwcTab *)c->pVtab)->p = ((struct fwcCsr *)c)->p = sqlite3_value_pointer(*a, "fw");
    --n, ++a;
  }
  if (!((struct fwcCsr *)c)->p)
    return (SQLITE_OK);
  if (n && x & 2) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwcCsr *)c)->p->d)
      ((struct fwcCsr *)c)->fe = i;
    else
      ((struct fwcCsr *)c)->fe = ((struct fwcCsr *)c)->p->d;
    ((struct fwcCsr *)c)->f = ((struct fwcCsr *)c)->fs = ((struct fwcCsr *)c)->fe - 1;
    --n, ++a;
  } else
    ((struct fwcCsr *)c)->fe = ((struct fwcCsr *)c)->p->d;
  if (n && x & 4) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwcCsr *)c)->p->d)
      ((struct fwcCsr *)c)->te = i;
    else
      ((struct fwcCsr *)c)->te = ((struct fwcCsr *)c)->p->d;
    ((struct fwcCsr *)c)->t = ((struct fwcCsr *)c)->ts = ((struct fwcCsr *)c)->te - 1;
    --n, ++a;
  } else
    ((struct fwcCsr *)c)->te = ((struct fwcCsr *)c)->p->d;
  if (n)
    return (SQLITE_ERROR);
  return (SQLITE_OK);
  (void)u1;
}

static int
fwcNxt(
  sqlite3_vtab_cursor *c
){
  if (++((struct fwcCsr *)c)->t >= ((struct fwcCsr *)c)->te) {
    if (++((struct fwcCsr *)c)->f >= ((struct fwcCsr *)c)->fe)
      return (SQLITE_OK);
    else
      ((struct fwcCsr *)c)->t = ((struct fwcCsr *)c)->ts;
  }
  return (SQLITE_OK);
}

static int
fwcEof(
  sqlite3_vtab_cursor *c
){
  if (((struct fwcCsr *)c)->f >= ((struct fwcCsr *)c)->fe)
    return (1); 
  else
    return (0); 
}

static int
fwcClm(
  sqlite3_vtab_cursor *c
 ,sqlite3_context *x
 ,int i
){
  switch (i) {
  case 0: /* f */
    sqlite3_result_int64(x, ((struct fwcCsr *)c)->f + 1);
    break;
  case 1: /* t */
    sqlite3_result_int64(x, ((struct fwcCsr *)c)->t + 1);
    break;
  case 2: /* c */
    sqlite3_result_int64(x, *(((struct fwcCsr *)c)->p->cst + ((struct fwcCsr *)c)->p->d * ((struct fwcCsr *)c)->f + ((struct fwcCsr *)c)->t));
    break;
  default:
    break;
  }
  return (SQLITE_OK);
}

static int
fwcRid(
  sqlite3_vtab_cursor *c
 ,sqlite3_int64 *i
){
  *i = 1 + ((struct fwcCsr *)c)->p->d * ((struct fwcCsr *)c)->f + ((struct fwcCsr *)c)->t;
  return (SQLITE_OK);
}

static int
fwcUpd(
  sqlite3_vtab *v
 ,int n
 ,sqlite3_value **a
 ,sqlite3_int64 *i
){
  sqlite3_int64 f;
  sqlite3_int64 t;
  sqlite3_int64 c;

  if (n != 6
   || !((struct fwcTab *)v)->p
   || sqlite3_value_type(*(a + 0)) == SQLITE_NULL
   || sqlite3_value_int64(*(a + 0)) != sqlite3_value_int64(*(a + 1))
   || (f = sqlite3_value_int64(*(a + 2)) - 1) < 0
   || f >= ((struct fwcTab *)v)->p->d
   || (t = sqlite3_value_int64(*(a + 3)) - 1) < 0
   || t >= ((struct fwcTab *)v)->p->d
   || (c = sqlite3_value_int64(*(a + 4))) < (fwCst_t)(1LU << (sizeof (fwCst_t) * 8 - 1))
   || c > (fwCst_t)((1LU << (sizeof (fwCst_t) * 8 - 1)) - 1)
   || (((struct fwcTab *)v)->p->nxt + ((struct fwcTab *)v)->p->d * f + t)->x
  )
    return (SQLITE_CONSTRAINT);
  *(((struct fwcTab *)v)->p->cst + ((struct fwcTab *)v)->p->d * f + t) = c;
  (((struct fwcTab *)v)->p->nxt + ((struct fwcTab *)v)->p->d * f + t)->l = t + 1;
  return (SQLITE_OK);
  (void)i;
}

sqlite3_module
fwcMod = {
  1,      /* iVersion */
  0,      /* xCreate */
  fwcCon, /* xConnect */
  fwcBst, /* xBestIndex */
  fwcDis, /* xDisconnect */
  0,      /* xDestroy */
  fwcOpn, /* xOpen */
  fwcCls, /* xClose */
  fwcFlt, /* xFilter */
  fwcNxt, /* xNext */
  fwcEof, /* xEof */
  fwcClm, /* xColumn */
  fwcRid, /* xRowid */
  fwcUpd, /* xUpdate */
  0,      /* xBegin */
  0,      /* xSync */
  0,      /* xCommit */
  0,      /* xRollback */
  0,      /* xFindMethod */
  0,      /* xRename */
  0,      /* xSavepoint */
  0,      /* xRelease */
  0,      /* xRollbackTo */
  0       /* xShadowName */
};

static int
fwnCon(
  sqlite3 *d
 ,void *u1
 ,int u2
 ,const char *const *u3
 ,sqlite3_vtab **v
 ,char **u4
){
  int i;

  if ((i = sqlite3_declare_vtab(d, "CREATE TABLE Fwn(f INTEGER, t INTEGER, o INTEGER, n INTEGER, p HIDDEN)")))
    return (i);
  if (!(*v = sqlite3_malloc(sizeof (**v))))
    return (SQLITE_NOMEM);
  sqlite3_vtab_config(d, SQLITE_VTAB_DIRECTONLY);
  return (SQLITE_OK);
  (void)u1;
  (void)u2;
  (void)u3;
  (void)u4;
}

static int
fwnDis(
  sqlite3_vtab *v
){
  sqlite3_free(v);
  return (SQLITE_OK);
}

struct fwnCsr {
  sqlite3_vtab_cursor c;
  struct fw *p;
  fwNxt_t f;
  fwNxt_t t;
  fwNxt_t o;
  fwNxt_t fs;
  fwNxt_t fe;
  fwNxt_t ts;
  fwNxt_t te;
  fwNxt_t os;
};

static int
fwnOpn(
  sqlite3_vtab *u1
 ,sqlite3_vtab_cursor **c
){
  struct fwnCsr *xc;

  if (!(xc = sqlite3_malloc(sizeof (*xc))))
    return (SQLITE_NOMEM);
  xc->p = 0;
  xc->f = xc->t = xc->o = xc->fs = xc->fe = xc->ts = xc->te = xc->os = 0;
  *c = &xc->c;
  return (SQLITE_OK);
  (void)u1;
}

static int
fwnCls(
  sqlite3_vtab_cursor *c
){
  sqlite3_free(c);
  return (SQLITE_OK);
}

static int
fwnBst(
  sqlite3_vtab *u1,
  sqlite3_index_info *i
){
  int n;
  int f;
  int t;
  int o;
  int p;

  f = t = o = p = -1;
  for (n = 0; n < i->nConstraint; ++n) {
    if (!(i->aConstraint + n)->usable)
      continue;
    switch ((i->aConstraint + n)->iColumn) {
    case 0: /* f */
      if ((i->aConstraint + n)->op != SQLITE_INDEX_CONSTRAINT_EQ)
        continue;
      f = n;
      break;
    case 1: /* t */
      if ((i->aConstraint + n)->op != SQLITE_INDEX_CONSTRAINT_EQ)
        continue;
      t = n;
      break;
    case 2: /* o */
      if ((i->aConstraint + n)->op != SQLITE_INDEX_CONSTRAINT_GE)
        continue;
      o = n;
      break;
#if 0
    case 3: /* n */
      break;
#endif
    case 4: /* p */
      p = n;
      break;
    default:
      break;
    }
  }
  n = 0;
  if (p >= 0) {
    (i->aConstraintUsage + p)->argvIndex = ++n;
    (i->aConstraintUsage + p)->omit = 1;
    i->idxNum |= 1;
    if (f >= 0) {
      (i->aConstraintUsage + f)->argvIndex = ++n;
      (i->aConstraintUsage + f)->omit = 1;
      i->idxNum |= 2;
    }
    if (t >= 0) {
      (i->aConstraintUsage + t)->argvIndex = ++n;
      (i->aConstraintUsage + t)->omit = 1;
      i->idxNum |= 4;
    }
    if (o >= 0) {
      (i->aConstraintUsage + o)->argvIndex = ++n;
      (i->aConstraintUsage + o)->omit = 1;
      i->idxNum |= 8;
    }
  }
  if (i->idxNum & 1) {
    if (i->idxNum & 2 && i->idxNum & 4 && i->idxNum & 8) {
      i->estimatedCost = 0.1;
      i->estimatedRows = 1;
    } else if (i->idxNum & 2 && i->idxNum & 4) {
      i->estimatedCost = 1.0;
      i->estimatedRows = 10;
    } else if (i->idxNum & 2 || i->idxNum & 4) {
      i->estimatedCost = 10.0;
      i->estimatedRows = 100;
    } else if (i->idxNum & 8) {
      i->estimatedCost = 100.0;
      i->estimatedRows = 1000;
    } else {
      i->estimatedCost = 1000.0;
      i->estimatedRows = 1000000;
    }
  }
  if (!i->idxNum)
    return (SQLITE_ERROR);
  if (i->nOrderBy == 3
   && (i->aOrderBy + 0)->iColumn == 0 && !(i->aOrderBy + 0)->desc
   && (i->aOrderBy + 1)->iColumn == 1 && !(i->aOrderBy + 1)->desc
   && (i->aOrderBy + 2)->iColumn == 2 && !(i->aOrderBy + 2)->desc)
    i->orderByConsumed = 1;
  else if (i->nOrderBy == 2
   && (i->aOrderBy + 0)->iColumn == 0 && !(i->aOrderBy + 0)->desc
   && (i->aOrderBy + 1)->iColumn == 1 && !(i->aOrderBy + 1)->desc)
    i->orderByConsumed = 1;
  else if (i->nOrderBy == 1
   && (i->aOrderBy + 0)->iColumn == 0 && !(i->aOrderBy + 0)->desc)
    i->orderByConsumed = 1;
  return (SQLITE_OK);
  (void)u1;
}

static int
fwnFlt(
  sqlite3_vtab_cursor *c
 ,int x
 ,const char *u1
 ,int n
 ,sqlite3_value **a
){
  sqlite3_int64 i;

  if (n && x & 1) {
    ((struct fwnCsr *)c)->p = sqlite3_value_pointer(*a, "fw");
    --n, ++a;
  }
  if (!((struct fwnCsr *)c)->p)
    return (SQLITE_OK);
  if (n && x & 2) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwnCsr *)c)->p->d)
      ((struct fwnCsr *)c)->fe = i;
    else
      ((struct fwnCsr *)c)->fe = ((struct fwnCsr *)c)->p->d;
    ((struct fwnCsr *)c)->f = ((struct fwnCsr *)c)->fs = ((struct fwnCsr *)c)->fe - 1;
    --n, ++a;
  } else
    ((struct fwnCsr *)c)->fe = ((struct fwnCsr *)c)->p->d;
  if (n && x & 4) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwnCsr *)c)->p->d)
      ((struct fwnCsr *)c)->te = i;
    else
      ((struct fwnCsr *)c)->te = ((struct fwnCsr *)c)->p->d;
    ((struct fwnCsr *)c)->t = ((struct fwnCsr *)c)->ts = ((struct fwnCsr *)c)->te - 1;
    --n, ++a;
  } else
    ((struct fwnCsr *)c)->te = ((struct fwnCsr *)c)->p->d;
  if (n && x & 8) {
    if ((i = sqlite3_value_int64(*a)) >= 0)
      ((struct fwnCsr *)c)->o = ((struct fwnCsr *)c)->os = i;
    --n, ++a;
  }
  if (n)
    return (SQLITE_ERROR);
  return (SQLITE_OK);
  (void)u1;
}

static int
fwnNxt(
  sqlite3_vtab_cursor *c
){
  if (++((struct fwnCsr *)c)->o
#ifdef FWEQL
   && (!(((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t)->x
   || ((((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t)->l <= ((struct fwnCsr *)c)->o))
#endif
  ) {
    ((struct fwnCsr *)c)->o = ((struct fwnCsr *)c)->os;
    if (++((struct fwnCsr *)c)->t >= ((struct fwnCsr *)c)->te) {
      if (++((struct fwnCsr *)c)->f >= ((struct fwnCsr *)c)->fe)
        return (SQLITE_OK);
      else
        ((struct fwnCsr *)c)->t = ((struct fwnCsr *)c)->ts;
    }
  }
  return (SQLITE_OK);
}

static int
fwnEof(
  sqlite3_vtab_cursor *c
){
  if (((struct fwnCsr *)c)->f >= ((struct fwnCsr *)c)->fe)
    return (1); 
  else
    return (0); 
}

static int
fwnClm(
  sqlite3_vtab_cursor *c
 ,sqlite3_context *x
 ,int i
){
  switch (i) {
  case 0: /* f */
    sqlite3_result_int64(x, ((struct fwnCsr *)c)->f + 1);
    break;
  case 1: /* t */
    sqlite3_result_int64(x, ((struct fwnCsr *)c)->t + 1);
    break;
  case 2: /* o */
    sqlite3_result_int64(x, ((struct fwnCsr *)c)->o);
    break;
  case 3: /* n */
#ifdef FWEQL
    if ((((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t)->x)
      sqlite3_result_int64(x, *((((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t)->x + ((struct fwnCsr *)c)->o));
    else
      sqlite3_result_int64(x, (((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t)->l);
#else
    sqlite3_result_int64(x, *(((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * ((struct fwnCsr *)c)->f + ((struct fwnCsr *)c)->t));
#endif
    break;
  default:
    break;
  }
  return (SQLITE_OK);
}

static int
fwnRid(
  sqlite3_vtab_cursor *c
 ,sqlite3_int64 *i
){
  fwNxt_t f;
  fwNxt_t t;

  *i = 1;
  for (f = 0; f < ((struct fwnCsr *)c)->f; ++f)
    for (t = 0; t < ((struct fwnCsr *)c)->p->d; ++t)
#ifdef FWEQL
      if ((((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * f + t)->x)
        *i += (((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * f + t)->l;
      else
#endif
        ++*i;
  for (t = 0; t < ((struct fwnCsr *)c)->t; ++t)
#ifdef FWEQL
    if ((((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * f + t)->x)
      *i += (((struct fwnCsr *)c)->p->nxt + ((struct fwnCsr *)c)->p->d * f + t)->l;
    else
#endif
      ++*i;
  *i += ((struct fwnCsr *)c)->o;
  return (SQLITE_OK);
}

sqlite3_module
fwnMod = {
  1,      /* iVersion */
  0,      /* xCreate */
  fwnCon, /* xConnect */
  fwnBst, /* xBestIndex */
  fwnDis, /* xDisconnect */
  0,      /* xDestroy */
  fwnOpn, /* xOpen */
  fwnCls, /* xClose */
  fwnFlt, /* xFilter */
  fwnNxt, /* xNext */
  fwnEof, /* xEof */
  fwnClm, /* xColumn */
  fwnRid, /* xRowid */
  0,      /* xUpdate */
  0,      /* xBegin */
  0,      /* xSync */
  0,      /* xCommit */
  0,      /* xRollback */
  0,      /* xFindMethod */
  0,      /* xRename */
  0,      /* xSavepoint */
  0,      /* xRelease */
  0,      /* xRollbackTo */
  0       /* xShadowName */
};

#endif /* FWSQLITE */

#ifdef FWMAIN

static void
mprint(
  const char *p
 ,struct fw *f
 ,fwNxt_t i
 ,fwNxt_t j
){
  char *s1;

  if (asprintf(&s1, "%s", p) < 0)
    return;
  for (;;) {
    char *s2;

    if (asprintf(&s2, "%s,%u|%d", s1, i, *(f->cst + f->d * i + j)) < 0) {
      free(s1);
      return;
    }
    free(s1);
    s1 = s2;
#ifdef FWEQL
    if ((f->nxt + f->d * i + j)->x) {
      fwNxt_t l;

      for (l = 0; l < (f->nxt + f->d * i + j)->l; ++l)
        mprint(s1, f, *((f->nxt + f->d * i + j)->x + l) - 1, j);
      free(s1);
      return;
    }
    i = (f->nxt + f->d * i + j)->l;
#else /* FWEQL */
    i = *(f->nxt + f->d * i + j);
#endif /* FWEQL */
    if (!i || --i == j)
      break;
  }
  printf("%s\n", s1);
  free(s1);
}

int
main(
  int argc
 ,char *argv[]
){
  struct fw *fw;
  unsigned long d;
  unsigned int t;
  fwNxt_t i;
  fwNxt_t j;
  fwCst_t c;
  int r;

  if (argc > 1 && !(t = strtoul(argv[1], 0, 10))) {
    fprintf(stderr, "Usage: %s [threads]\n", argv[0]);
    return (1);
  } else
    t = 1;

#ifdef FWBLK
  chanInit(realloc, free);
#endif /* FWBLK */

  if (scanf("vertices %lu\n", &d) != 1 || !d) {
    fprintf(stderr, "!d\n");
    exit(EXIT_FAILURE);
  }

#ifdef FWBLK
  /* blocking size should fit Process0 in processor cache */
# ifdef FWEQL
  fw = fwAlloc(d, 16);
# else /* FWEQL */
  fw = fwAlloc(d, 32);
# endif /* FWEQL */
#else /* FWBLK */
  fw = fwAlloc(d);
#endif /* FWBLK */
  if (!fw) {
    fprintf(stderr, "fwAlloc\n");
    exit(EXIT_FAILURE);
  }

  if (!scanf("edges\n"))
    while (scanf("%u %u %d\n", &i, &j, &c) == 3) {
      if (i >= d || j >= d)
        continue;
      *(fw->cst + fw->d * i + j) = c;
#ifdef FWEQL
      (fw->nxt + fw->d * i + j)->l = j + 1;
#else /* FWEQL */
      *(fw->nxt + fw->d * i + j) = j + 1;
#endif /* FWEQL */
    }

#ifdef FWBLK
  r = fwProcess(fw, t);
#else /* FWBLK */
  r = fwProcess(fw);
#endif /* FWBLK */
  if (r) {
    fwFree(fw);
    fprintf(stderr, "fwProcess %d\n", r);
    exit(EXIT_FAILURE);
  }

  if (!scanf("show\n"))
    while (scanf("%u %u\n", &i, &j) == 2) {
      char *s;

      if (i >= d || j >= d)
        continue;
      if (asprintf(&s, "%u:%u", i, j) > 0) {
        mprint(s, fw, i, j);
        free(s);
      }
    }

  fwFree(fw);
  return (0);
}

#endif /* FWMAIN */
