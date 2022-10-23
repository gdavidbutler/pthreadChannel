/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2022 G. David Butler <gdb@dbSystems.com>
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
  if (d > 1U << (sizeof (fwNxt_t) * 8 - 1)) /* problem too big for next element size */
    return (0);
  i = d;
  d *= d;
  if (!(fw = calloc(1, sizeof (*fw)))
   || !(fw->cst = malloc(d * sizeof (*fw->cst)))
   || !(fw->nxt = calloc(d, sizeof (*fw->nxt)))) {
    if (fw) {
      free(fw->cst);
      free(fw);
    }
    return (0);
  }
  fw->d = i;
  for (i = 0; i < fw->d; ++i)
    for (j = 0; j < fw->d; ++j)
      if (i == j)
        *(fw->cst + fw->d * i + j) = 0;
      else
        *(fw->cst + fw->d * i + j) = (1U << (sizeof (fwCst_t) * 8 - 1)) - 1;
#ifdef FWBLK
  fw->b = b;
#endif /* FWBLK */
  return (fw);
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

  if (v->d <= v->b)
    return (fwProcess0(v->cst, v->cst, v->cst, v->nxt, v->nxt, v->d, v->d));
  r = 1;
  if ((c = v->d / v->b) < p)
    p = c;
  c = v->b * (c / p);
  if (p > 1) {
    if (!(fwt.snk = chanCreate(0,0,0)) || !(fwt.src = chanCreate(0,0,0))) {
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
        goto exit;;
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
  return (fwProcess0(v->cst, v->cst, v->cst, v->nxt, v->nxt, v->d, v->d));
}
#endif /* FWBLK */

#ifdef FWSQLITE

static int
fwCon(
  sqlite3 *d
 ,void *u1
 ,int u2
 ,const char *const *u3
 ,sqlite3_vtab **v
 ,char **u4
){
  int i;

  if ((i = sqlite3_declare_vtab(d, "CREATE TABLE FWT(f INTEGER, t INTEGER, o INTEGER, n INTEGER, p HIDDEN)")))
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
fwDis(
  sqlite3_vtab *v
){
  sqlite3_free(v);
  return (SQLITE_OK);
}

struct fwCsr {
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
fwOpn(
  sqlite3_vtab *u1
 ,sqlite3_vtab_cursor **c
){
  struct fwCsr *xc;

  if (!(xc = sqlite3_malloc(sizeof (*xc))))
    return (SQLITE_NOMEM);
  xc->p = 0;
  xc->f = xc->t = xc->o = xc->fs = xc->fe = xc->ts = xc->te = xc->os = 0;
  *c = &xc->c;
  return (SQLITE_OK);
  (void)u1;
}

static int
fwCls(
  sqlite3_vtab_cursor *c
){
  sqlite3_free(c);
  return (SQLITE_OK);
}

static int
fwBst(
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
fwFlt(
  sqlite3_vtab_cursor *c
 ,int x
 ,const char *u1
 ,int n
 ,sqlite3_value **a
){
  sqlite3_int64 i;

  if (n && x & 1) {
    ((struct fwCsr *)c)->p = sqlite3_value_pointer(*a, "fw");
    --n, ++a;
  }
  if (n && x & 2) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwCsr *)c)->p->d)
      ((struct fwCsr *)c)->fe = i;
    else
      ((struct fwCsr *)c)->fe = ((struct fwCsr *)c)->p->d;
    ((struct fwCsr *)c)->f =((struct fwCsr *)c)->fs = ((struct fwCsr *)c)->fe - 1;
    --n, ++a;
  } else
    ((struct fwCsr *)c)->fe = ((struct fwCsr *)c)->p->d;
  if (n && x & 4) {
    if ((i = sqlite3_value_int64(*a)) >= 1
     && i <= ((struct fwCsr *)c)->p->d)
      ((struct fwCsr *)c)->te = i;
    else
      ((struct fwCsr *)c)->te = ((struct fwCsr *)c)->p->d;
    ((struct fwCsr *)c)->t =((struct fwCsr *)c)->ts = ((struct fwCsr *)c)->te - 1;
    --n, ++a;
  } else
    ((struct fwCsr *)c)->te = ((struct fwCsr *)c)->p->d;
  if (n && x & 8) {
    if ((i = sqlite3_value_int64(*a)) >= 0)
      ((struct fwCsr *)c)->o = ((struct fwCsr *)c)->os = i;
    --n, ++a;
  }
  if (n || !(((struct fwCsr *)c)->p))
    return (SQLITE_ERROR);
  return (SQLITE_OK);
  (void)u1;
}

static int
fwNxt(
  sqlite3_vtab_cursor *c
){
  if (++((struct fwCsr *)c)->o
#ifdef FWEQL
   && (!(((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)->x
   || ((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)->l <= ((struct fwCsr *)c)->o))
#endif
  ) {
    ((struct fwCsr *)c)->o = ((struct fwCsr *)c)->os;
    if (++((struct fwCsr *)c)->t >= ((struct fwCsr *)c)->te) {
      if (++((struct fwCsr *)c)->f >= ((struct fwCsr *)c)->fe)
        return (SQLITE_OK);
      else
        ((struct fwCsr *)c)->t = ((struct fwCsr *)c)->ts;
    }
  }
  return (SQLITE_OK);
}

static int
fwEof(
  sqlite3_vtab_cursor *c
){
  if (((struct fwCsr *)c)->f >= ((struct fwCsr *)c)->fe)
    return (1); 
  else
    return (0); 
}

static int
fwRid(
  sqlite3_vtab_cursor *c
 ,sqlite3_int64 *i
){
  fwNxt_t f;
  fwNxt_t t;

  *i = 1;
  for (f = 0; f < ((struct fwCsr *)c)->f; ++f)
    for (t = 0; t < ((struct fwCsr *)c)->p->d; ++t)
#ifdef FWEQL
      if ((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * f + t)->x)
        *i += (((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * f + t)->l;
      else
#endif
        ++*i;
  for (t = 0; t < ((struct fwCsr *)c)->t; ++t)
#ifdef FWEQL
    if ((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * f + t)->x)
      *i += (((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * f + t)->l;
    else
#endif
      ++*i;
  *i += ((struct fwCsr *)c)->o;
  return (SQLITE_OK);
}

static int
fwClm(
  sqlite3_vtab_cursor *c
 ,sqlite3_context *x
 ,int i
){
  switch (i) {
  case 0: /* f */
    sqlite3_result_int64(x, ((struct fwCsr *)c)->f + 1);
    break;
  case 1: /* t */
    sqlite3_result_int64(x, ((struct fwCsr *)c)->t + 1);
    break;
  case 2: /* o */
    sqlite3_result_int64(x, ((struct fwCsr *)c)->o);
    break;
  case 3: /* n */
#ifdef FWEQL
    if ((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)->x)
      sqlite3_result_int64(x, *((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)->x + ((struct fwCsr *)c)->o));
    else
      sqlite3_result_int64(x, (((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)->l);
#else
    sqlite3_result_int64(x, *((((struct fwCsr *)c)->p->nxt + ((struct fwCsr *)c)->p->d * ((struct fwCsr *)c)->f + ((struct fwCsr *)c)->t)));
#endif
    break;
  default:
    break;
  }
  return (SQLITE_OK);
}

sqlite3_module
fwMod = {
  0,     /* iVersion */
  0,     /* xCreate */
  fwCon, /* xConnect */
  fwBst, /* xBestIndex */
  fwDis, /* xDisconnect */
  0,     /* xDestroy */
  fwOpn, /* xOpen */
  fwCls, /* xClose */
  fwFlt, /* xFilter */
  fwNxt, /* xNext */
  fwEof, /* xEof */
  fwClm, /* xColumn */
  fwRid, /* xRowid */
  0,     /* xUpdate */
  0,     /* xBegin */
  0,     /* xSync */
  0,     /* xCommit */
  0,     /* xRollback */
  0,     /* xFindMethod */
  0,     /* xRename */
  0,     /* xSavepoint */
  0,     /* xRelease */
  0,     /* xRollbackTo */
  0      /* xShadowName */
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

    if (asprintf(&s2, "%s,%u", s1, i) < 0) {
      free(s1);
      return;
    }
    free(s1);
    s1 = s2;
    if (i == j)
      break;
#ifdef FWEQL
    if ((f->nxt + f->d * i + j)->x) {
      fwNxt_t l;

      for (l = 0; l < (f->nxt + f->d * i + j)->l; ++l)
        mprint(s1, f, *((f->nxt + f->d * i + j)->x + l) - 1, j);
      free(s1);
      return;
    }
    i = (f->nxt + f->d * i + j)->l - 1;
#else /* FWEQL */
    i = *(f->nxt + f->d * i + j) - 1;
#endif /* FWEQL */
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
