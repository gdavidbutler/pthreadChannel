#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
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

static int
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
  void *tv;
#endif /* FWEQL */
  fwNxt_t k;
  fwNxt_t i;
  fwNxt_t j;
  fwNxt_t l;
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
              if (!(tv = malloc((an + d * i + k)->l * sizeof (*cn->x))))
                return (1);
              (cn + d * i + j)->x = tv;
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
  if (p > 1) {
    if (v->d < 128) /* multi-thread break */
      p = 1;
    else if (!(fwt.snk = chanCreate(0,0,0))
     || !(fwt.src = chanCreate(0,0,0))) {
      chanClose(fwt.snk);
      return (r);
    }
  }
  if ((c = v->d / v->b) < p)
    p = c;
  c = v->b * (c / p);
  if (!(fwp = malloc(p * sizeof (*fwp))))
    goto exit;
  for (i = 0; i < p - 1; ++i) {
    pthread_t t;

    chanOpen(fwt.snk);
    chanOpen(fwt.src);
    if (pthread_create(&t, 0, fwThread, &fwt)) {
      chanClose(fwt.src);
      chanClose(fwt.snk);
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
# ifdef FWEQL
  fw = fwAlloc(d, 16); /* blocking size by experimentation */
# else /* FWEQL */
  fw = fwAlloc(d, 32); /* blocking size by experimentation */
# endif /* FWEQL */
#else /* FWBLK */
  fw = fwAlloc(d);
#endif /* FWBLK */
  if (!fw) {
    fprintf(stderr, "fwAlloc\n");
    exit(EXIT_FAILURE);
  }

  if (!scanf("edges\n"))
    while (scanf("%u %u %hd\n", &i, &j, &c) == 3) {
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
