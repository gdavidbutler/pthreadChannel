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

#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include "chan.h"

static void *(*ChanA)(void *, unsigned long);
static void (*ChanF)(void *);

void
chanInit(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
){
  f(a(0,1)); /* force exception here and now */
  ChanA = a;
  ChanF = f;
}

/* internal thread rendezvous */
typedef struct {
  chan_t **s;        /* signaled chans */
  unsigned int ss;   /* signaled size */
  unsigned int st;   /* signaled tail */
  unsigned int c;    /* chan queue reference count */
  int e;             /* thread exists */
  int w;             /* thread is waiting */
  pthread_mutex_t m;
  pthread_condattr_t a;
  pthread_cond_t r;
} cpr_t;

static void
dCpr(
  cpr_t *p
){
  if (p->e || p->c)
    pthread_mutex_unlock(&p->m);
  else {
    pthread_cond_destroy(&p->r);
    pthread_condattr_destroy(&p->a);
    pthread_mutex_unlock(&p->m);
    pthread_mutex_destroy(&p->m);
    ChanF(p->s);
    ChanF(p);
  }
}

static void
cdCpr(
  void *v
){
  pthread_mutex_lock(&((cpr_t *)v)->m);
  ((cpr_t *)v)->e = 0;
  dCpr((cpr_t *)v);
}

static pthread_key_t Cpr;

static void
cCpr(
  void
){
  pthread_key_create(&Cpr, cdCpr);
}

static cpr_t *
gCpr(
  void
){
  static pthread_once_t o = PTHREAD_ONCE_INIT;
  cpr_t *p;

  if (pthread_once(&o, cCpr))
    return (0);
  if (!(p = pthread_getspecific(Cpr))) {
    if (!(p = ChanA(0, sizeof (*p)))
     || pthread_mutex_init(&p->m, 0)) {
      ChanF(p);
      return (0);
    }
    if (pthread_condattr_init(&p->a)) {
      pthread_mutex_destroy(&p->m);
      ChanF(p);
      return (0);
    }
#ifdef HAVE_CONDATTR_SETCLOCK
    pthread_condattr_setclock(&p->a, CLOCK_MONOTONIC);
#endif
    if (pthread_cond_init(&p->r, &p->a)) {
      pthread_condattr_destroy(&p->a);
      pthread_mutex_destroy(&p->m);
      ChanF(p);
      return (0);
    }
    p->s = 0;
    p->ss = p->st = 0;
    p->c = 0;
    p->e = 1;
    p->w = 0;
    if (pthread_setspecific(Cpr, p)) {
      pthread_cond_destroy(&p->r);
      pthread_condattr_destroy(&p->a);
      pthread_mutex_destroy(&p->m);
      ChanF(p);
      return (0);
    }
  }
  return (p);
}

/* chan */
struct chan {
  chanSd_t d;      /* store deallocation function */
  chanSi_t i;      /* store implementation function */
  void *v;         /* if s, store context else value */
  cpr_t **g;       /* get circular queue */
  cpr_t **p;       /* put circular queue */
  cpr_t **e;       /* get event circular queue */
  cpr_t **u;       /* put event circular queue */
  cpr_t **h;       /* shutdown event circular queue */
  unsigned int gs; /* queue size */
  unsigned int gh; /* queue head */
  unsigned int gt; /* queue tail */
  unsigned int ps; /* queue size */
  unsigned int ph; /* queue head */
  unsigned int pt; /* queue tail */
  unsigned int es; /* queue size */
  unsigned int eh; /* queue head */
  unsigned int et; /* queue tail */
  unsigned int us; /* queue size */
  unsigned int uh; /* queue head */
  unsigned int ut; /* queue tail */
  unsigned int hs; /* queue size */
  unsigned int hh; /* queue head */
  unsigned int ht; /* queue tail */
  unsigned int c;  /* open count */
  unsigned int l;  /* below chan bit flags */
  chanSs_t t;      /* store status */
  pthread_mutex_t m;
};

/* chan bit flags */
/* NOTE: chanGe and chanPe map to chanSw_t */
static const unsigned int chanGe = 0x01; /* is empty to differentiate h==t */
static const unsigned int chanPe = 0x02; /* is empty to differentiate h==t */
static const unsigned int chanEe = 0x04; /* is empty to differentiate h==t */
static const unsigned int chanUe = 0x08; /* is empty to differentiate h==t */
static const unsigned int chanHe = 0x10; /* is empty to differentiate h==t */
static const unsigned int chanSu = 0x80; /* is shutdown */

/* find "me" in a queue else make room if needed and ... */
#define FIND_ELSE(F,V,G) do {\
  if (!(c->l & F)) {\
    k = c->V##h;\
    do {\
      if (*(c->V + k) == m)\
        goto G;\
      if (++k == c->V##s)\
        k = 0;\
    } while (k != c->V##t);\
    if (c->V##h == c->V##t) {\
      if (!(v = ChanA(c->V, (c->V##s + 1) * sizeof (*c->V))))\
        goto exit;\
      c->V = v;\
      if ((k = c->V##s - c->V##h))\
        for (q = c->V + c->V##h + k; k; --k, --q)\
          *q = *(q - 1);\
      ++c->V##h;\
      ++c->V##s;\
    }\
  }\
} while (0)

/* ... insert "me" at the tail of the queue */
#define INSERT_AT_TAIL(F,V) do {\
  ++m->c;\
  *(c->V + c->V##t) = m;\
  if (c->V##s != 1 && ++c->V##t == c->V##s)\
    c->V##t = 0;\
  c->l &= ~F;\
} while (0)

/* ... insert "me" at the head of the queue */
#define INSERT_AT_HEAD(F,V) do {\
  ++m->c;\
  if (!c->V##h)\
    c->V##h = c->V##s;\
  *(c->V + --c->V##h) = m;\
  c->l &= ~F;\
} while (0)

/* dequeue and wakeup "other" thread(s) */
#define WAKE(F,V,W,B) do {\
  while (!(c->l & F) && W) {\
    p = *(c->V + c->V##h);\
    if (++c->V##h == c->V##s)\
      c->V##h = 0;\
    if (c->V##h == c->V##t)\
      c->l |= F;\
    if (p == m) {\
      --p->c;\
      continue;\
    } else {\
      pthread_mutex_lock(&p->m);\
      --p->c;\
    }\
    if (p->w && !pthread_cond_signal(&p->r)) {\
      for (l = 0; l < p->st && *(p->s + l) != c; ++l);\
      if (l == p->st && l < p->ss)\
        *(p->s + p->st++) = c;\
      pthread_mutex_unlock(&p->m);\
      B\
    } else\
      dCpr(p);\
  }\
} while (0)

/* Store callback to set chanSs_t and wake threads */
static int
chanWake(
  chan_t *c
 ,chanSs_t s
){
  cpr_t *m;
  cpr_t *p;
  unsigned int l;

  if (!c)
    return (1);
  pthread_mutex_lock(&c->m);
  if (!c->i || c->l & chanSu) {
    pthread_mutex_unlock(&c->m);
    return (1);
  }
  m = 0;
  if (!s) {
    c->l |= chanSu;
    WAKE(chanGe, g, 1, ;);
    WAKE(chanPe, p, 1, ;);
    WAKE(chanEe, e, 1, ;);
    WAKE(chanUe, u, 1, ;);
    WAKE(chanHe, h, 1, ;);
  } else {
    if (s & chanSsCanGet && !(c->t & chanSsCanGet)) {
      c->t |= chanSsCanGet;
      WAKE(chanGe, g, c->t & chanSsCanGet, break;);
    }
    if (s & chanSsCanPut && !(c->t & chanSsCanPut)) {
      c->t |= chanSsCanPut;
      WAKE(chanPe, p, c->t & chanSsCanPut, break;);
    }
  }
  pthread_mutex_unlock(&c->m);
  return (0);
}

chan_t *
chanCreate(
  void (*q)(void*)
 ,chanSa_t a
 ,...
){
  chan_t *c;

  if (!ChanA || !ChanF || !(c = ChanA(0, sizeof (*c))))
    return (0);
  c->p = c->e = c->u = c->h = 0;
  if (!(c->g = ChanA(0, sizeof (*c->g)))
   || !(c->p = ChanA(0, sizeof (*c->p)))
   || !(c->e = ChanA(0, sizeof (*c->e)))
   || !(c->u = ChanA(0, sizeof (*c->u)))
   || !(c->h = ChanA(0, sizeof (*c->h)))
   || pthread_mutex_init(&c->m, 0)) {
error:
    ChanF(c->h);
    ChanF(c->u);
    ChanF(c->e);
    ChanF(c->p);
    ChanF(c->g);
    ChanF(c);
    return (0);
  }
  c->d = 0;
  c->i = 0;
  if (a) {
    va_list l;

    va_start(l, a);
    c->t = a(ChanA, ChanF, q, (int(*)(void*,chanSs_t))chanWake, c, &c->d, &c->i, &c->v, l);
    va_end(l);
    if (!c->t || !c->i) {
      pthread_mutex_destroy(&c->m);
      goto error;
    }
  } else {
    c->d = (chanSd_t)q;
    c->t = chanSsCanPut;
  }
  c->gs = c->ps = c->es = c->us = c->hs = 1;
  c->gh = c->ph = c->eh = c->uh = c->hh = 0;
  c->gt = c->pt = c->et = c->ut = c->ht = 0;
  c->c = 0;
  c->l = chanGe | chanPe | chanEe | chanUe | chanHe;
  return (c);
}

chan_t *
chanOpen(
  chan_t *c
){
  if (c) {
    pthread_mutex_lock(&c->m);
    ++c->c;
    pthread_mutex_unlock(&c->m);
  }
  return (c);
}

void
chanShut(
  chan_t *c
){
  cpr_t *m;
  cpr_t *p;
  unsigned int l;

  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->l & chanSu) {
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->l |= chanSu;
  m = 0;
  WAKE(chanGe, g, 1, ;);
  WAKE(chanPe, p, 1, ;);
  WAKE(chanEe, e, 1, ;);
  WAKE(chanUe, u, 1, ;);
  WAKE(chanHe, h, 1, ;);
  pthread_mutex_unlock(&c->m);
}

void
chanClose(
  chan_t *c
){
  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->c) {
    --c->c;
    pthread_mutex_unlock(&c->m);
    return;
  }
  while ((c->l & (chanGe | chanPe | chanEe | chanUe | chanHe)) != (chanGe | chanPe | chanEe | chanUe | chanHe)) {
    cpr_t *m;
    cpr_t *p;
    unsigned int l;

    m = 0;
    WAKE(chanGe, g, 1, ;);
    WAKE(chanPe, p, 1, ;);
    WAKE(chanEe, e, 1, ;);
    WAKE(chanUe, u, 1, ;);
    WAKE(chanHe, h, 1, ;);
    pthread_mutex_unlock(&c->m);
    sched_yield();
    pthread_mutex_lock(&c->m);
  }
  ChanF(c->g);
  ChanF(c->p);
  ChanF(c->e);
  ChanF(c->u);
  ChanF(c->h);
  pthread_mutex_unlock(&c->m);
  if (c->d && (c->i || c->t & chanSsCanGet))
    c->d(c->v, c->t);
  pthread_mutex_destroy(&c->m);
  ChanF(c);
}

unsigned int
chanOpenCnt(
  chan_t *c
){
  unsigned int r;

  if (!c)
    return (0);
  pthread_mutex_lock(&c->m);
  r = c->c;
  pthread_mutex_unlock(&c->m);
  return (r);
}

chanOs_t
chanOp(
  long w
 ,chan_t *c
 ,void **v
 ,chanOp_t o
){
  chanArr_t p[1];

  p[0].c = c;
  p[0].v = v;
  p[0].o = o;
  if (chanOne(w, sizeof (p) / sizeof (p[0]), p))
    return p[0].s;
  return chanOsNop;
}

unsigned int
chanOne(
  long w
 ,unsigned int t
 ,chanArr_t *a
){
  chan_t *c;
  cpr_t *m;
  cpr_t *p;
  cpr_t **q;
  void *v;
  unsigned int i;
  unsigned int j;
  unsigned int k;
  unsigned int l;
  struct timespec s;

  if (!t || !a)
    return (0);
  m = 0;
  j = 0;
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
sht1:
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      return (i + 1);
    }
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (!(a + i)->v) {
      if (c->l & chanSu)
        goto sht1;
      else if (!(c->l & chanPe))
        goto get2;
    } else {
      if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))) {
get1:
        if (c->i)
          c->t = c->i(c->v, chanSoGet, c->l & (chanGe | chanPe), (a + i)->v);
        else {
          *((a + i)->v) = c->v;
          c->t = chanSsCanPut;
        }
        k = 0;
        WAKE(chanPe, p, c->t & chanSsCanPut, k=1;break;);
        if (!k && !(c->l & chanGe))
          WAKE(chanUe, u, 1, break;);
get2:
        pthread_mutex_unlock(&c->m);
        if (!c->t)
          chanShut(c);
        (a + i)->s = chanOsGet;
        return (i + 1);
      } else if (!(c->t & chanSsCanGet) && c->l & chanSu)
        goto sht1;
    }
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu)
      goto sht1;
    if (!(a + i)->v) {
      if (!(c->l & chanGe))
        goto put2;
    } else {
      if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
put1:
        if (c->i)
          c->t = c->i(c->v, chanSoPut, c->l & (chanGe | chanPe), (a + i)->v);
        else {
          c->v = *((a + i)->v);
          c->t = chanSsCanGet;
        }
        k = 0;
        WAKE(chanGe, g, c->t & chanSsCanGet, k=1;break;);
        if (!k && !(c->l & chanPe))
          WAKE(chanEe, e, 1, break;);
put2:
        pthread_mutex_unlock(&c->m);
        if (!c->t)
          chanShut(c);
        (a + i)->s = chanOsPut;
        return (i + 1);
      }
    }
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (!j || w < 0) {
    if (j)
      (a + (j - 1))->s = chanOsTmo;
    return (j);
  }
lock1:
  j = 0;
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    if (!j) {
      pthread_mutex_lock(&c->m);
      j = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      goto fnd1;
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!j) {
      pthread_mutex_lock(&c->m);
      j = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (!(a + i)->v) {
      if (c->l & chanSu)
        goto fnd1;
      else if (!(c->l & chanPe))
        goto fnd1;
    } else {
      if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe)))
        goto fnd1;
      else if (!(c->t & chanSsCanGet) && c->l & chanSu)
        goto fnd1;
    }
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!j) {
      pthread_mutex_lock(&c->m);
      j = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      goto fnd1;
    if (!(a + i)->v) {
      if (!(c->l & chanGe))
        goto fnd1;
    } else {
      if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe)))
        goto fnd1;
    }
    break;
  }
  if (i < t) {
unlock1:
    while (i) switch ((a + --i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
    case chanOpGet:
    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_unlock(&c->m);
      break;
    }
    sched_yield();
    goto lock1;
fnd1:
    j = i;
    while (j) switch ((a + --j)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
    case chanOpGet:
    case chanOpPut:
      if (!(c = (a + j)->c))
        break;
      pthread_mutex_unlock(&c->m);
      break;
    }
    c = (a + i)->c;
    switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (c->l & chanSu)
        goto sht1;
      break;

    case chanOpGet:
      if (!(a + i)->v) {
        if (c->l & chanSu)
          goto sht1;
        else if (!(c->l & chanPe))
          goto get2;
      } else {
        if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe)))
          goto get1;
        else if (!(c->t & chanSsCanGet) && c->l & chanSu)
          goto sht1;
      }
      break;

    case chanOpPut:
      if (c->l & chanSu)
        goto sht1;
      if (!(a + i)->v) {
        if (!(c->l & chanGe))
          goto put2;
      } else {
        if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe)))
          goto put1;
      }
      break;
    }
    pthread_mutex_unlock(&c->m);
    return (0);
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
  case chanOpSht:
    break;

  case chanOpGet:
    if ((c = (a + i)->c) && (a + i)->v && c->l & chanGe)
      WAKE(chanUe, u, 1, break;);
    break;

  case chanOpPut:
    if ((c = (a + i)->c) && (a + i)->v && c->l & chanPe)
      WAKE(chanEe, e, 1, break;);
    break;
  }
  if (!(m = gCpr()))
    goto exit;
  pthread_mutex_lock(&m->m);
  if (t > m->ss) {
    if (!(v = ChanA(m->s, t * sizeof (*m->s))))
      goto exit;
    m->s = v;
    m->ss = t;
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    FIND_ELSE(chanHe, h, fndSht1);
    INSERT_AT_TAIL(chanHe, h);
fndSht1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE(chanEe, e, fndGet1);
      INSERT_AT_TAIL(chanEe, e);
    } else {
      FIND_ELSE(chanGe, g, fndGet1);
      INSERT_AT_TAIL(chanGe, g);
    }
fndGet1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE(chanUe, u, fndPut1);
      INSERT_AT_TAIL(chanUe, u);
    } else {
      FIND_ELSE(chanPe, p, fndPut1);
      INSERT_AT_TAIL(chanPe, p);
    }
fndPut1:
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (w > 0) {
    static long nsps = 1000000000L;

    if (
#ifdef HAVE_CONDATTR_SETCLOCK
        clock_gettime(CLOCK_MONOTONIC, &s)
#else
        clock_gettime(CLOCK_REALTIME, &s)
#endif
    )
      goto exit;
    if (w > nsps) {
      s.tv_sec += w / nsps;
      s.tv_nsec += w % nsps;
    } else
      s.tv_nsec += w;
    if (s.tv_nsec > nsps) {
      ++s.tv_sec;
      s.tv_nsec -= nsps;
    }
  }
  for (;;) {
    m->w = 1;
    m->st = 0;
    if (w > 0) {
      if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
        m->w = 0;
        pthread_mutex_unlock(&m->m);
        for (i = 0; i < t && ((a + i)->o == chanOpNop || !(a + i)->c); ++i);
        if (i < t) {
          (a + i)->s = chanOsTmo;
          return (i + 1);
        } else
          return (0);
      }
    } else
      pthread_cond_wait(&m->r, &m->m);
    m->w = 0;
    pthread_mutex_unlock(&m->m);
lock2:
    j = 0;
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      if (!j) {
        pthread_mutex_lock(&c->m);
        j = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        goto fnd2;
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!j) {
        pthread_mutex_lock(&c->m);
        j = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (!(a + i)->v) {
        if (c->l & chanSu)
          goto fnd2;
        else if (!(c->l & chanPe))
          goto fnd2;
      } else {
        if (c->t & chanSsCanGet)
          goto fnd2;
        else if (c->l & chanSu)
          goto fnd2;
      }
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!j) {
        pthread_mutex_lock(&c->m);
        j = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        goto fnd2;
      if (!(a + i)->v) {
        if (!(c->l & chanGe))
          goto fnd2;
      } else {
        if (c->t & chanSsCanPut)
          goto fnd2;
      }
      break;
    }
    if (i < t) {
unlock2:
      while (i) switch ((a + --i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
      case chanOpGet:
      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        pthread_mutex_unlock(&c->m);
        break;
      }
      sched_yield();
      goto lock2;
fnd2:
      j = i;
      for (i = 0; i < t; ++i) if (i == j) continue; else switch ((a + i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
        if (!(c = (a + i)->c))
          break;
        if (i < j)
          pthread_mutex_unlock(&c->m);
        break;

      case chanOpGet:
        if (!(c = (a + i)->c))
          break;
        for (k = 0; k < m->st && *(m->s + k) != c; ++k);
        if (k < m->st) {
          if (i > j)
            pthread_mutex_lock(&c->m);
          if (!(a + i)->v)
            WAKE(chanEe, e, 1, break;);
          else
            WAKE(chanGe, g, c->t & chanSsCanGet, break;);
          pthread_mutex_unlock(&c->m);
        } else if (i < j)
          pthread_mutex_unlock(&c->m);
        break;

      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        for (k = 0; k < m->st && *(m->s + k) != c; ++k);
        if (k < m->st) {
          if (i > j)
            pthread_mutex_lock(&c->m);
          if (!(a + i)->v)
            WAKE(chanUe, u, 1, break;);
          else
            WAKE(chanPe, p, c->t & chanSsCanPut, break;);
          pthread_mutex_unlock(&c->m);
        } else if (i < j)
          pthread_mutex_unlock(&c->m);
        break;
      }
      i = j;
      c = (a + i)->c;
      switch ((a + i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
        if (c->l & chanSu)
          goto sht1;
        break;

      case chanOpGet:
        if (!(a + i)->v) {
          if (c->l & chanSu)
            goto sht1;
          else if (!(c->l & chanPe))
            goto get2;
        } else {
          if (c->t & chanSsCanGet)
            goto get1;
          else if (c->l & chanSu)
            goto sht1;
        }
        break;

      case chanOpPut:
        if (c->l & chanSu)
          goto sht1;
        if (!(a + i)->v) {
          if (!(c->l & chanGe))
            goto put2;
        } else {
          if (c->t & chanSsCanPut)
            goto put1;
        }
        break;
      }
      pthread_mutex_unlock(&c->m);
      return (0);
    }
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
    case chanOpSht:
      break;

    case chanOpGet:
      if ((c = (a + i)->c) && (a + i)->v && c->l & chanGe)
        WAKE(chanUe, u, 1, break;);
      break;

    case chanOpPut:
      if ((c = (a + i)->c) && (a + i)->v && c->l & chanPe)
        WAKE(chanEe, e, 1, break;);
      break;
    }
    pthread_mutex_lock(&m->m);
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      FIND_ELSE(chanHe, h, fndSht2);
      INSERT_AT_HEAD(chanHe, h);
fndSht2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE(chanEe, e, fndGet2);
        INSERT_AT_HEAD(chanEe, e);
      } else {
        FIND_ELSE(chanGe, g, fndGet2);
        INSERT_AT_HEAD(chanGe, g);
      }
fndGet2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE(chanUe, u, fndPut2);
        INSERT_AT_HEAD(chanUe, u);
      } else {
        FIND_ELSE(chanPe, p, fndPut2);
        INSERT_AT_HEAD(chanPe, p);
      }
fndPut2:
      pthread_mutex_unlock(&c->m);
      break;
    }
  }
exit:
  if (m)
    pthread_mutex_unlock(&m->m);
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
  case chanOpGet:
  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_unlock(&c->m);
    break;
  }
  return (0);
}

chanAl_t
chanAll(
  long w
 ,unsigned int t
 ,chanArr_t *a
){
  chan_t *c;
  cpr_t *m;
  cpr_t *p;
  cpr_t **q;
  void *v;
  unsigned int i;
  unsigned int j;
  unsigned int k;
  unsigned int l;
  struct timespec s;

  if (!t || !a)
    return (chanAlErr);
  m = 0;
lock1:
  j = 0;
  k = 0;
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    if (!k) {
      pthread_mutex_lock(&c->m);
      k = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      j |= 2;
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!k) {
      pthread_mutex_lock(&c->m);
      k = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (!(a + i)->v) {
      if (c->l & chanSu)
        j |= 2;
      else if (!(c->l & chanPe))
        j |= 2;
      else
        j |= 1;
    } else {
      if (!(c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))))
        j |= 1;
      else if (!(c->t & chanSsCanGet) && c->l & chanSu)
        j |= 2;
    }
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!k) {
      pthread_mutex_lock(&c->m);
      k = 1;
    } else if (pthread_mutex_trylock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      j |= 2;
    else {
      if (!(a + i)->v) {
        if (!(c->l & chanGe))
          j |= 2;
        else
          j |= 1;
      } else {
        if (!(c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))))
          j |= 1;
      }
    }
    break;
  }
  if (i < t) {
unlock1:
    while (i) switch ((a + --i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
    case chanOpGet:
    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_unlock(&c->m);
      break;
    }
    sched_yield();
    goto lock1;
  }
  if (j & 2) {
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      if (c->l & chanSu)
        (a + i)->s = chanOsSht;
      else
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (c->l & chanSu)
        (a + i)->s = chanOsSht;
      else if (!(a + i)->v) {
        if (!(c->l & chanPe))
          (a + i)->s = chanOsGet;
        else
          (a + i)->s = chanOsNop;
      } else
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (c->l & chanSu)
        (a + i)->s = chanOsSht;
      else if (!(a + i)->v) {
        if (!(c->l & chanGe))
          (a + i)->s = chanOsPut;
        else
          (a + i)->s = chanOsNop;
      } else
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;
    }
    return (chanAlEvt);
  }
  if (!j || w < 0) {
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v)
        goto get1;
      else if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))) {
        if (c->i)
          c->t = c->i(c->v, chanSoGet, c->l & (chanGe | chanPe), (a + i)->v);
        else {
          *((a + i)->v) = c->v;
          c->t = chanSsCanPut;
        }
        k = 0;
        WAKE(chanPe, p, c->t & chanSsCanPut, k=1;break;);
        if (!k && !(c->l & chanGe)) {
          if (c->t & chanSsCanGet)
            WAKE(chanGe, g, c->t & chanSsCanGet, break;);
          else
            WAKE(chanUe, u, 1, break;);
        }
        (a + i)->s = chanOsGet;
      } else
get1:
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      if (!c->t)
        chanShut(c);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v)
        goto put1;
      else if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
        if (c->i)
          c->t = c->i(c->v, chanSoPut, c->l & (chanGe | chanPe), (a + i)->v);
        else {
          c->v = *((a + i)->v);
          c->t = chanSsCanGet;
        }
        k = 0;
        WAKE(chanGe, g, c->t & chanSsCanGet, k=1;break;);
        if (!k && !(c->l & chanPe)) {
          if (c->t & chanSsCanPut)
            WAKE(chanPe, p, c->t & chanSsCanPut, break;);
          else
            WAKE(chanEe, e, 1, break;);
        }
        (a + i)->s = chanOsPut;
      } else
put1:
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      if (!c->t)
        chanShut(c);
      break;
    }
    return (chanAlOp);
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
  case chanOpSht:
    break;

  case chanOpGet:
    if ((c = (a + i)->c) && (a + i)->v && c->l & chanGe)
      WAKE(chanUe, u, 1, break;);
    break;

  case chanOpPut:
    if ((c = (a + i)->c) && (a + i)->v && c->l & chanPe)
      WAKE(chanEe, e, 1, break;);
    break;
  }
  if (!(m = gCpr()))
    goto exit;
  pthread_mutex_lock(&m->m);
  if (t > m->ss) {
    if (!(v = ChanA(m->s, t * sizeof (*m->s))))
      goto exit;
    m->s = v;
    m->ss = t;
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    FIND_ELSE(chanHe, h, fndSht1);
    INSERT_AT_TAIL(chanHe, h);
fndSht1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE(chanEe, e, fndGet1);
      INSERT_AT_TAIL(chanEe, e);
    } else {
      FIND_ELSE(chanGe, g, fndGet1);
      INSERT_AT_TAIL(chanGe, g);
    }
fndGet1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE(chanUe, u, fndPut1);
      INSERT_AT_TAIL(chanUe, u);
    } else {
      FIND_ELSE(chanPe, p, fndPut1);
      INSERT_AT_TAIL(chanPe, p);
    }
fndPut1:
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (w > 0) {
    static long nsps = 1000000000L;

    if (
#ifdef HAVE_CONDATTR_SETCLOCK
        clock_gettime(CLOCK_MONOTONIC, &s)
#else
        clock_gettime(CLOCK_REALTIME, &s)
#endif
    )
      goto exit;
    if (w > nsps) {
      s.tv_sec += w / nsps;
      s.tv_nsec += w % nsps;
    } else
      s.tv_nsec += w;
    if (s.tv_nsec > nsps) {
      ++s.tv_sec;
      s.tv_nsec -= nsps;
    }
  }
  for (;;) {
    m->w = 1;
    m->st = 0;
    if (w > 0) {
      if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
        m->w = 0;
        pthread_mutex_unlock(&m->m);
        return (chanAlTmo);
      }
    } else
      pthread_cond_wait(&m->r, &m->m);
    m->w = 0;
    pthread_mutex_unlock(&m->m);
lock2:
    j = 0;
    k = 0;
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      if (!k) {
        pthread_mutex_lock(&c->m);
        k = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        j |= 2;
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!k) {
        pthread_mutex_lock(&c->m);
        k = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (!(a + i)->v) {
        if (c->l & chanSu)
          j |= 2;
        else if (!(c->l & chanPe))
          j |= 2;
        else
          j |= 1;
      } else {
        if (!(c->t & chanSsCanGet))
          j |= 1;
        else if (c->l & chanSu)
          j |= 2;
      }
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!k) {
        pthread_mutex_lock(&c->m);
        k = 1;
      } else if (pthread_mutex_trylock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        j |= 2;
      else {
        if (!(a + i)->v) {
          if (!(c->l & chanGe))
            j |= 2;
          else
            j |= 1;
        } else {
          if (!(c->t & chanSsCanPut))
            j |= 1;
        }
      }
      break;
    }
    if (i < t) {
unlock2:
      while (i) switch ((a + --i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
      case chanOpGet:
      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        pthread_mutex_unlock(&c->m);
        break;
      }
      sched_yield();
      goto lock2;
    }
    if (j & 2) {
      for (i = 0; i < t; ++i) switch ((a + i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
        if (!(c = (a + i)->c))
          break;
        if (c->l & chanSu)
          (a + i)->s = chanOsSht;
        else
          (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;

      case chanOpGet:
        if (!(c = (a + i)->c))
          break;
        for (k = 0; k < m->st && *(m->s + k) != c; ++k);
        if (k < m->st) {
          if (!(a + i)->v)
            WAKE(chanEe, e, 1, break;);
          else
            WAKE(chanGe, g, c->t & chanSsCanGet, break;);
        }
        if (c->l & chanSu)
          (a + i)->s = chanOsSht;
        else if (!(a + i)->v) {
          if (!(c->l & chanPe))
            (a + i)->s = chanOsGet;
          else
            (a + i)->s = chanOsNop;
        } else
          (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;

      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        for (k = 0; k < m->st && *(m->s + k) != c; ++k);
        if (k < m->st) {
          if (!(a + i)->v)
            WAKE(chanUe, u, 1, break;);
          else
            WAKE(chanPe, p, c->t & chanSsCanPut, break;);
        }
        if (c->l & chanSu)
          (a + i)->s = chanOsSht;
        else if (!(a + i)->v) {
          if (!(c->l & chanGe))
            (a + i)->s = chanOsPut;
          else
            (a + i)->s = chanOsNop;
        } else
          (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;
      }
      return (chanAlEvt);
    }
    if (!j) {
      for (i = 0; i < t; ++i) switch ((a + i)->o) {

      case chanOpNop:
        break;

      case chanOpSht:
        if (!(c = (a + i)->c))
          break;
        (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;

      case chanOpGet:
        if (!(c = (a + i)->c))
          break;
        if (c->t & chanSsCanGet && (a + i)->v) {
          if (c->i)
            c->t = c->i(c->v, chanSoGet, c->l & (chanGe | chanPe), (a + i)->v);
          else {
            *((a + i)->v) = c->v;
            c->t = chanSsCanPut;
          }
          k = 0;
          WAKE(chanPe, p, c->t & chanSsCanPut, k=1;break;);
          if (!k && !(c->l & chanGe)) {
            if (c->t & chanSsCanGet)
              WAKE(chanGe, g, c->t & chanSsCanGet, break;);
            else
              WAKE(chanUe, u, 1, break;);
          }
          (a + i)->s = chanOsGet;
        } else {
          for (k = 0; k < m->st && *(m->s + k) != c; ++k);
          if (k < m->st) {
            if (!(a + i)->v)
              WAKE(chanEe, e, 1, break;);
            else
              WAKE(chanGe, g, c->t & chanSsCanGet, break;);
          }
          (a + i)->s = chanOsNop;
        }
        pthread_mutex_unlock(&c->m);
        if (!c->t)
          chanShut(c);
        break;

      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        if (c->t & chanSsCanPut && (a + i)->v) {
          if (c->i)
            c->t = c->i(c->v, chanSoPut, c->l & (chanGe | chanPe), (a + i)->v);
          else {
            c->v = *((a + i)->v);
            c->t = chanSsCanGet;
          }
          k = 0;
          WAKE(chanGe, g, c->t & chanSsCanGet, k=1;break;);
          if (!k && !(c->l & chanPe)) {
            if (c->t & chanSsCanPut)
              WAKE(chanPe, p, c->t & chanSsCanPut, break;);
            else
              WAKE(chanEe, e, 1, break;);
          }
          (a + i)->s = chanOsPut;
        } else {
          for (k = 0; k < m->st && *(m->s + k) != c; ++k);
          if (k < m->st) {
            if (!(a + i)->v)
              WAKE(chanUe, u, 1, break;);
            else
              WAKE(chanPe, p, c->t & chanSsCanPut, break;);
          }
          (a + i)->s = chanOsNop;
        }
        pthread_mutex_unlock(&c->m);
        if (!c->t)
          chanShut(c);
        break;
      }
      return (chanAlOp);
    }
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
    case chanOpSht:
      break;

    case chanOpGet:
      if ((c = (a + i)->c) && (a + i)->v && c->l & chanGe)
        WAKE(chanUe, u, 1, break;);
      break;

    case chanOpPut:
      if ((c = (a + i)->c) && (a + i)->v && c->l & chanPe)
        WAKE(chanEe, e, 1, break;);
      break;
    }
    pthread_mutex_lock(&m->m);
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      FIND_ELSE(chanHe, h, fndSht2);
      INSERT_AT_HEAD(chanHe, h);
fndSht2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE(chanEe, e, fndGet2);
        INSERT_AT_HEAD(chanEe, e);
      } else {
        FIND_ELSE(chanGe, g, fndGet2);
        INSERT_AT_HEAD(chanGe, g);
      }
fndGet2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE(chanUe, u, fndPut2);
        INSERT_AT_HEAD(chanUe, u);
      } else {
        FIND_ELSE(chanPe, p, fndPut2);
        INSERT_AT_HEAD(chanPe, p);
      }
fndPut2:
      pthread_mutex_unlock(&c->m);
      break;
    }
  }
exit:
  if (m)
    pthread_mutex_unlock(&m->m);
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
  case chanOpGet:
  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_unlock(&c->m);
    break;
  }
  return (chanAlErr);
}
