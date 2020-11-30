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

#include <pthread.h>
#include "chan.h"

/* internal rendezvous */
typedef struct {
  void (*f)(void *);
  unsigned int c;    /* reference count */
  int t;             /* thread is active */
  int w;             /* thread is waiting */
  pthread_mutex_t m;
  pthread_condattr_t a;
  pthread_cond_t r;
} cpr_t;

static pthread_key_t Cpr;

static void
dCpr(
  cpr_t *p
){
  if (p->t || p->c)
    pthread_mutex_unlock(&p->m);
  else {
    pthread_cond_destroy(&p->r);
    pthread_condattr_destroy(&p->a);
    pthread_mutex_unlock(&p->m);
    pthread_mutex_destroy(&p->m);
    p->f(p);
  }
}

static void
cdCpr(
  void *v
){
  pthread_mutex_lock(&((cpr_t *)v)->m);
  ((cpr_t *)v)->t = ((cpr_t *)v)->w = 0;
  dCpr((cpr_t *)v);
}

static void
cCpr(
  void
){
  pthread_key_create(&Cpr, cdCpr);
}

static cpr_t *
gCpr(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
){
  static pthread_once_t o = PTHREAD_ONCE_INIT;
  cpr_t *p;

  if (pthread_once(&o, cCpr))
    return (0);
  if (!(p = pthread_getspecific(Cpr))) {
    if (!(p = a(0, sizeof (*p)))
     || pthread_mutex_init(&p->m, 0)) {
      f(p);
      return (0);
    }
    p->f = f;
    if (pthread_condattr_init(&p->a)) {
      pthread_mutex_destroy(&p->m);
      p->f(p);
      return (0);
    }
#ifdef HAVE_CONDATTR_SETCLOCK
    pthread_condattr_setclock(&p->a, CLOCK_MONOTONIC);
#endif
    if (pthread_cond_init(&p->r, &p->a)) {
      pthread_condattr_destroy(&p->a);
      pthread_mutex_destroy(&p->m);
      p->f(p);
      return (0);
    }
    p->t = 1;
    p->w = 0;
    p->c = 0;
    if (pthread_setspecific(Cpr, p)) {
      pthread_cond_destroy(&p->r);
      pthread_condattr_destroy(&p->a);
      pthread_mutex_destroy(&p->m);
      p->f(p);
      return (0);
    }
  }
  pthread_mutex_lock(&p->m);
  return (p);
}

/* bit flags */
static const unsigned int chanSu = 0x01; /* is shutdown */
static const unsigned int chanEe = 0x02; /* event queue empty, to differentiate eh==et */
static const unsigned int chanGe = 0x04; /* get queue empty,   to differentiate gh==gt */
static const unsigned int chanPe = 0x08; /* put queue empty,   to differentiate ph==pt */

/* chan */
struct chan {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chanSi_t s;      /* store implementation function */
  chanSd_t d;      /* store done function */
  void *v;         /* if s, store context else value */
  cpr_t **e;       /* event circular queue */
  cpr_t **g;       /* get circular queue */
  cpr_t **p;       /* put circular queue */
  unsigned int es; /* get event queue size */
  unsigned int eh; /* get event queue head */
  unsigned int et; /* get event queue tail */
  unsigned int gs; /* get queue size */
  unsigned int gh; /* get queue head */
  unsigned int gt; /* get queue tail */
  unsigned int ps; /* put queue size */
  unsigned int ph; /* put queue head */
  unsigned int pt; /* put queue tail */
  unsigned int c;  /* open count */
  unsigned int l;  /* above bit flags */
  chanSs_t t;      /* store status */
  pthread_mutex_t m;
};

chan_t *
chanCreate(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chanSi_t s
 ,void *v
 ,chanSd_t d
){
  chan_t *c;

  if (!(c = a(0, sizeof (*c))))
    return (0);
  c->e = c->g = c->p = 0;
  if (!(c->e = a(0, sizeof (*c->e)))
   || !(c->g = a(0, sizeof (*c->g)))
   || !(c->p = a(0, sizeof (*c->p)))
   || pthread_mutex_init(&c->m, 0)) {
    if (c->p)
      f(c->p);
    if (c->g)
      f(c->g);
    if (c->e)
      f(c->e);
    f(c);
    return (0);
  }
  c->a = a;
  c->f = f;
  c->s = s;
  c->d = d;
  c->v = v;
  c->es = c->gs = c->ps = 1;
  c->eh = c->et = c->gh = c->gt = c->ph = c->pt = 0;
  c->c = 1;
  c->l = chanEe | chanGe | chanPe;
  c->t = chanSsCanPut;
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

/* a "template" to manually "inline" code */
/* wakeup "other" thread(s) */
#define WAKE(F,V,WE,IE,B) do {\
  int f = 0;\
  while (!(c->l & F) WE) {\
    p = *(c->V + c->V##h);\
    if (p != m && pthread_mutex_trylock(&p->m)) {\
      pthread_mutex_unlock(&c->m);\
      pthread_mutex_lock(&p->m);\
      if (pthread_mutex_trylock(&c->m)) {\
        pthread_mutex_unlock(&p->m);\
        pthread_mutex_lock(&c->m);\
        continue;\
      }\
      if (c->l & F IE || p != *(c->V + c->V##h)) {\
        pthread_mutex_unlock(&p->m);\
        continue;\
      }\
    }\
    if (++c->V##h == c->V##s)\
      c->V##h = 0;\
    if (c->V##h == c->V##t)\
      c->l |= F;\
    --p->c;\
    if (p == m) {\
      f = 1;\
      continue;\
    }\
    if (p->w && !pthread_cond_signal(&p->r)) {\
      pthread_mutex_unlock(&p->m);\
      B\
    } else\
      dCpr(p);\
  }\
  if (f) {\
    ++m->c;\
    if (!c->V##h)\
      c->V##h = c->V##s;\
    *(c->V + --c->V##h) = m;\
    c->l &= ~F;\
  }\
} while (0)

void
chanShut(
  chan_t *c
){
  cpr_t *p;
  cpr_t *m;

  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->l & chanSu) {
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->l |= chanSu;
  m = 0;
  WAKE(chanEe, e, , , );
  WAKE(chanGe, g, , , );
  WAKE(chanPe, p, , , );
  pthread_mutex_unlock(&c->m);
}

void
chanClose(
  chan_t *c
){
  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->c > 1) {
    --c->c;
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->f(c->p);
  c->f(c->g);
  c->f(c->e);
  if (c->d)
    c->d(c->v);
  pthread_mutex_unlock(&c->m);
  pthread_mutex_destroy(&c->m);
  c->f(c);
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

/* a "template" to manually "inline" code */
/* find "me" in a queue else insert "me" ... */
#define FIND_ELSE_INSERT(F,V,G) do {\
  if (!(c->l & F)) {\
    k = c->V##h;\
    do {\
      if (*(c->V + k) == m)\
        goto G;\
      if (++k == c->V##s)\
        k = 0;\
    } while (k != c->V##t);\
    if (c->V##h == c->V##t) {\
      if (!(v = c->a(c->V, (c->V##s + 1) * sizeof (*c->V)))) {\
        pthread_mutex_unlock(&c->m);\
        goto exit;\
      }\
      c->V = v;\
      if ((k = c->V##s - c->V##h))\
        for (q = c->V + c->V##h + k; k; --k, --q)\
          *q = *(q - 1);\
      ++c->V##h;\
      ++c->V##s;\
    }\
  }\
} while (0)
/* ... at the tail of the queue */
#define AT_TAIL(F,V) do {\
  ++m->c;\
  *(c->V + c->V##t) = m;\
  if (c->V##s != 1 && ++c->V##t == c->V##s)\
    c->V##t = 0;\
  c->l &= ~F;\
} while (0)
/* ... at the head of the queue */
#define AT_HEAD(F,V) do {\
  ++m->c;\
  if (!c->V##h)\
    c->V##h = c->V##s;\
  *(c->V + --c->V##h) = m;\
  c->l &= ~F;\
} while (0)

unsigned int
chanOne(
  long w
 ,unsigned int t
 ,chanArr_t *a
){
  chan_t *c;
  cpr_t *p;
  cpr_t *m;
  cpr_t **q;
  void *v;
  unsigned int i;
  unsigned int j;
  unsigned int k;
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
      if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
        goto get2;
      else if (c->l & chanSu)
        goto sht1;
      else
        goto get3;
    }
    if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))) {
get1:
      if (c->s)
        c->t = c->s(c->v, chanSoGet, c->l & (chanGe|chanPe), (a + i)->v);
      else {
        *((a + i)->v) = c->v;
        c->t = chanSsCanPut;
      }
      k = 0;
      WAKE(chanPe, p, && c->t & chanSsCanPut, || !(c->t & chanSsCanPut), k=1;break;);
      if (!k && !(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
        WAKE(chanEe, e, , , );
get2:
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsGet;
      return (i + 1);
    }
    if (!(c->t & chanSsCanGet) && c->l & chanSu)
      goto sht1;
get3:
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
      if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
        goto put2;
      else
        goto put3;
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
put1:
      if (c->s)
        c->t = c->s(c->v, chanSoPut, c->l & (chanGe|chanPe), (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->t = chanSsCanGet;
      }
      k = 0;
      WAKE(chanGe, g, && c->t & chanSsCanGet, || !(c->t & chanSsCanGet), k=1;break;);
      if (!k && !(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
        WAKE(chanEe, e, , , );
put2:
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsPut;
      return (i + 1);
    }
put3:
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (!j || w < 0) {
    if (j)
      (a + (j - 1))->s = chanOsTmo;
    return (j);
  }
  for (i = 0; i < t && !(a + i)->c; ++i);
  if (i == t || !(m = gCpr((a + i)->c->a, (a + i)->c->f))) {
    m = 0;
    goto exit;
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&m->m);
      goto sht1;
    }
    FIND_ELSE_INSERT(chanEe, e, fndSht1);
    AT_TAIL(chanEe, e);
fndSht1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_lock(&c->m);
    if (!(a + i)->v) {
      if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe) {
        pthread_mutex_unlock(&m->m);
        goto get2;
      } else if (c->l & chanSu) {
        pthread_mutex_unlock(&m->m);
        goto sht1;
      } else {
        FIND_ELSE_INSERT(chanEe, e, fndGet1);
        AT_TAIL(chanEe, e);
        goto fndGet1;
      }
    }
    if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))) {
      pthread_mutex_unlock(&m->m);
      goto get1;
    }
    if (!(c->t & chanSsCanGet) && c->l & chanSu) {
      pthread_mutex_unlock(&m->m);
      goto sht1;
    }
    FIND_ELSE_INSERT(chanGe, g, fndGet1);
    AT_TAIL(chanGe, g);
    if (!(c->t & chanSsCanGet) && c->l & chanPe)
      WAKE(chanEe, e, , , );
fndGet1:
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&m->m);
      goto sht1;
    }
    if (!(a + i)->v) {
      if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe) {
        pthread_mutex_unlock(&m->m);
        goto put2;
      } else {
        FIND_ELSE_INSERT(chanEe, e, fndPut1);
        AT_TAIL(chanEe, e);
        goto fndPut1;
      }
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
      pthread_mutex_unlock(&m->m);
      goto put1;
    }
    FIND_ELSE_INSERT(chanPe, p, fndPut1);
    AT_TAIL(chanPe, p);
    if (!(c->t & chanSsCanPut) && c->l & chanGe)
      WAKE(chanEe, e, , , );
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
    if (w > 0) {
      if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
        m->w = 0;
        pthread_mutex_unlock(&m->m);
        if (j)
          (a + (j - 1))->s = chanOsTmo;
        return (j);
      }
    } else
      pthread_cond_wait(&m->r, &m->m);
    m->w = 0;
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&m->m);
        goto sht1;
      }
      FIND_ELSE_INSERT(chanEe, e, fndSht2);
      AT_HEAD(chanEe, e);
fndSht2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_lock(&c->m);
      if (!(a + i)->v) {
        if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe) {
          pthread_mutex_unlock(&m->m);
          goto get2;
        } else if (c->l & chanSu) {
          pthread_mutex_unlock(&m->m);
          goto sht1;
        } else {
          FIND_ELSE_INSERT(chanEe, e, fndGet2);
          AT_HEAD(chanEe, e);
          goto fndGet2;
        }
      }
      if (c->t & chanSsCanGet) {
        pthread_mutex_unlock(&m->m);
        goto get1;
      }
      if (c->l & chanSu) {
        pthread_mutex_unlock(&m->m);
        goto sht1;
      }
      FIND_ELSE_INSERT(chanGe, g, fndGet2);
      AT_HEAD(chanGe, g);
      if (!(c->t & chanSsCanGet) && c->l & chanPe)
        WAKE(chanEe, e, , , );
fndGet2:
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&m->m);
        goto sht1;
      }
      if (!(a + i)->v) {
        if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe) {
          pthread_mutex_unlock(&m->m);
          goto put2;
        } else {
          FIND_ELSE_INSERT(chanEe, e, fndPut2);
          AT_HEAD(chanEe, e);
          goto fndPut2;
        }
      }
      if (c->t & chanSsCanPut) {
        pthread_mutex_unlock(&m->m);
        goto put1;
      }
      FIND_ELSE_INSERT(chanPe, p, fndPut2);
      AT_HEAD(chanPe, p);
      if (!(c->t & chanSsCanPut) && c->l & chanGe)
        WAKE(chanEe, e, , , );
fndPut2:
      pthread_mutex_unlock(&c->m);
      break;
    }
  }
exit:
  if (m)
    pthread_mutex_unlock(&m->m);
  return (0);
}

chanMs_t
chanAll(
  long w
 ,unsigned int t
 ,chanArr_t *a
){
  chan_t *c;
  cpr_t *p;
  cpr_t *m;
  cpr_t **q;
  void *v;
  unsigned int i;
  unsigned int j;
  unsigned int k;
  struct timespec s;

  if (!t || !a)
    return (chanMsErr);
  for (i = 0; i < t && !(a + i)->c; ++i);
  if (i == t || !(m = gCpr((a + i)->c->a, (a + i)->c->f)))
    return (chanMsErr);
  j = 0;
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    if (pthread_mutex_lock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      j |= 2;
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (pthread_mutex_lock(&c->m))
      goto unlock1;
    if (!(a + i)->v) {
      if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
        j |= 2;
      else if (c->l & chanSu)
        j |= 2;
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
    if (pthread_mutex_lock(&c->m))
      goto unlock1;
    if (c->l & chanSu)
      j |= 2;
    else {
      if (!(a + i)->v) {
        if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
          j |= 2;
      } else {
        if (!(c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))))
          j |= 1;
      }
    }
    break;
  }
  if (i < t) {
unlock1:
    pthread_mutex_unlock(&m->m);
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
    return (chanMsErr);
  }
  if (j & 2) {
doSht:
    pthread_mutex_unlock(&m->m);
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      if ((c->l & chanSu))
        (a + i)->s = chanOsSht;
      else
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
          (a + i)->s = chanOsGet;
        else if (c->l & chanSu)
          (a + i)->s = chanOsSht;
        else
          (a + i)->s = chanOsNop;
      } else {
        if (!(c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))))
          (a + i)->s = chanOsNop;
        else if (!(c->t & chanSsCanGet) && c->l & chanSu)
          (a + i)->s = chanOsSht;
        else
          (a + i)->s = chanOsNop;
      }
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if ((c->l & chanSu))
        (a + i)->s = chanOsSht;
      else if (!(a + i)->v) {
        if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
          (a + i)->s = chanOsPut;
        else
          (a + i)->s = chanOsNop;
      } else
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;
    }
    return (chanMsEvt);
  }
  if (!j || w < 0) {
    pthread_mutex_unlock(&m->m);
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
      if (c->t & chanSsCanGet && (c->l & chanGe || !(c->l & chanPe))) {
        if (c->s)
          c->t = c->s(c->v, chanSoGet, c->l & (chanGe|chanPe), (a + i)->v);
        else {
          *((a + i)->v) = c->v;
          c->t = chanSsCanPut;
        }
        k = 0;
        WAKE(chanPe, p, && c->t & chanSsCanPut, || !(c->t & chanSsCanPut), k=1;break;);
        if (!k && !(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
          WAKE(chanEe, e, , , );
        (a + i)->s = chanOsGet;
      } else
get1:
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v)
        goto put1;
      if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
        if (c->s)
          c->t = c->s(c->v, chanSoPut, c->l & (chanGe|chanPe), (a + i)->v);
        else {
          c->v = *((a + i)->v);
          c->t = chanSsCanGet;
        }
        k = 0;
        WAKE(chanGe, g, && c->t & chanSsCanGet, || !(c->t & chanSsCanGet), k=1;break;);
        if (!k && !(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
          WAKE(chanEe, e, , , );
        (a + i)->s = chanOsPut;
      } else
put1:
        (a + i)->s = chanOsNop;
      pthread_mutex_unlock(&c->m);
      break;
    }
    return (chanMsOp);
  }
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNop:
    break;

  case chanOpSht:
    if (!(c = (a + i)->c))
      break;
    FIND_ELSE_INSERT(chanEe, e, fndSht1);
    AT_TAIL(chanEe, e);
fndSht1:
    break;

  case chanOpGet:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE_INSERT(chanEe, e, fndGet1);
      AT_TAIL(chanEe, e);
      goto fndGet1;
    }
    FIND_ELSE_INSERT(chanGe, g, fndGet1);
    AT_TAIL(chanGe, g);
fndGet1:
    break;

  case chanOpPut:
    if (!(c = (a + i)->c))
      break;
    if (!(a + i)->v) {
      FIND_ELSE_INSERT(chanEe, e, fndPut1);
      AT_TAIL(chanEe, e);
      goto fndPut1;
    }
    FIND_ELSE_INSERT(chanPe, p, fndPut1);
    AT_TAIL(chanPe, p);
fndPut1:
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
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpGet:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      if (!(c->t & chanSsCanGet) && c->l & chanPe)
        WAKE(chanEe, e, , , );
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpPut:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      if (!(c->t & chanSsCanPut) && c->l & chanGe)
        WAKE(chanEe, e, , , );
      pthread_mutex_unlock(&c->m);
      break;
    }
    m->w = 1;
    if (w > 0) {
      if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
        m->w = 0;
        pthread_mutex_unlock(&m->m);
        return (chanMsTmo);
      }
    } else
      pthread_cond_wait(&m->r, &m->m);
    m->w = 0;
    j = 0;
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      if (pthread_mutex_lock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        j |= 2;
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (pthread_mutex_lock(&c->m))
        goto unlock2;
      if (!(a + i)->v) {
        if (!(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
          j |= 2;
        else if (c->l & chanSu)
          j |= 2;
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
      if (pthread_mutex_lock(&c->m))
        goto unlock2;
      if (c->l & chanSu)
        j |= 2;
      else {
        if (!(a + i)->v) {
          if (!(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
            j |= 2;
        } else {
          if (!(c->t & chanSsCanPut))
            j |= 1;
        }
      }
      break;
    }
    if (i < t) {
unlock2:
      goto unlock1;
    }
    if (j & 2)
      goto doSht;
    if (!j) {
      pthread_mutex_unlock(&m->m);
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
          goto get2;
        if (c->t & chanSsCanGet) {
          if (c->s)
            c->t = c->s(c->v, chanSoGet, c->l & (chanGe|chanPe), (a + i)->v);
          else {
            *((a + i)->v) = c->v;
            c->t = chanSsCanPut;
          }
          k = 0;
          WAKE(chanPe, p, && c->t & chanSsCanPut, || !(c->t & chanSsCanPut), k=1;break;);
          if (!k && !(c->t & chanSsCanGet) && !(c->l & chanGe) && c->l & chanPe)
            WAKE(chanEe, e, , , );
          (a + i)->s = chanOsGet;
        } else
get2:
          (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;

      case chanOpPut:
        if (!(c = (a + i)->c))
          break;
        if (!(a + i)->v)
          goto put2;
        if (c->t & chanSsCanPut) {
          if (c->s)
            c->t = c->s(c->v, chanSoPut, c->l & (chanGe|chanPe), (a + i)->v);
          else {
            c->v = *((a + i)->v);
            c->t = chanSsCanGet;
          }
          k = 0;
          WAKE(chanGe, g, && c->t & chanSsCanGet, || !(c->t & chanSsCanGet), k=1;break;);
          if (!k && !(c->t & chanSsCanPut) && !(c->l & chanPe) && c->l & chanGe)
            WAKE(chanEe, e, , , );
          (a + i)->s = chanOsPut;
        } else
put2:
          (a + i)->s = chanOsNop;
        pthread_mutex_unlock(&c->m);
        break;
      }
      return (chanMsOp);
    }
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNop:
      break;

    case chanOpSht:
      if (!(c = (a + i)->c))
        break;
      FIND_ELSE_INSERT(chanEe, e, fndSht2);
      AT_HEAD(chanEe, e);
fndSht2:
      break;

    case chanOpGet:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE_INSERT(chanEe, e, fndGet2);
        AT_HEAD(chanEe, e);
        goto fndGet2;
      }
      FIND_ELSE_INSERT(chanGe, g, fndGet2);
      AT_HEAD(chanGe, g);
fndGet2:
      break;

    case chanOpPut:
      if (!(c = (a + i)->c))
        break;
      if (!(a + i)->v) {
        FIND_ELSE_INSERT(chanEe, e, fndPut2);
        AT_HEAD(chanEe, e);
        goto fndPut2;
      }
      FIND_ELSE_INSERT(chanPe, p, fndPut2);
      AT_HEAD(chanPe, p);
fndPut2:
      break;
    }
  }
exit:
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
  return (chanMsErr);
}
