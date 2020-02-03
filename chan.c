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

#include <stdlib.h> /* for abort() and to support chanCreate(0,0 ,...) to indicate realloc() and free() */
#include <pthread.h>
#include "chan.h"

/*
 * Design note: A sleeping thread (on pthread_cond_wait) may be
 * cancelled, cleanup delegated to threads attempting wakeup
 */

/* internal chanPoll rendezvous */
typedef struct {
  void (*f)(void *);
  int t;             /* thread is active */
  int w;             /* thread is waiting */
  unsigned int c;    /* reference count */
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
  if (pthread_key_create(&Cpr, cdCpr))
    abort();
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
  return (p);
}

/* chan */
struct chan {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  chanSi_t q;      /* store implementation function */
  chanSd_t d;      /* store done function */
  void *v;         /* if q, store context else value */
  cpr_t **s;       /* shutdown circular queue */
  cpr_t **g;       /* getter circular queue */
  cpr_t **p;       /* putter circular queue */
  unsigned int ss; /* shutdown queue size */
  unsigned int sh; /* shutdown queue head */
  unsigned int st; /* shutdown queue tail */
  unsigned int gs; /* getter queue size */
  unsigned int gh; /* getter queue head */
  unsigned int gt; /* getter queue tail */
  unsigned int ps; /* putter queue size */
  unsigned int ph; /* putter queue head */
  unsigned int pt; /* putter queue tail */
  unsigned int c;  /* open count */
  unsigned int l;  /* below flags: chanSe, chanGe, chanPe and chanSu */
  chanSs_t t;      /* store status */
  pthread_mutex_t m;
};

static const unsigned int chanSe = 1; /* shutdown queue empty, to differentiate sh==st */
static const unsigned int chanGe = 2; /* getter queue empty, to differentiate gh==gt */
static const unsigned int chanPe = 4; /* putter queue empty, to differentiate ph==pt */
static const unsigned int chanSu = 8; /* is shutdown */

chan_t *
chanCreate(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chanSi_t q
 ,void *v
 ,chanSd_t d
){
  chan_t *c;

  if ((a || f) && (!a || !f))
    return (0);
  if (a) {
    /* force exceptions here and now */
    if (!(c = a(0, sizeof (*c))))
      return (0);
    f(c);
    if (!(c = a(0, sizeof (*c))))
      return (0);
    c->a = a;
    c->f = f;
  } else {
    if (!(c = realloc(0, sizeof (*c))))
      return (0);
    c->a = realloc;
    c->f = free;
  }
  c->g = c->p = 0;
  if (!(c->s = c->a(0, sizeof (*c->s)))
   || !(c->g = c->a(0, sizeof (*c->g)))
   || !(c->p = c->a(0, sizeof (*c->p)))
   || pthread_mutex_init(&c->m, 0)) {
    c->f(c->p);
    c->f(c->g);
    c->f(c->s);
    c->f(c);
    return (0);
  }
  c->ss = c->gs = c->ps = 1;
  c->sh = c->st = c->gh = c->gt = c->ph = c->pt = 0;
  c->l = chanSe | chanGe | chanPe;
  if ((c->q = q)) {
    c->v = v;
    c->d = d;
  }
  c->c = 1;
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

void
chanShut(
  chan_t *c
){
  cpr_t *p;

  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->l & chanSu) {
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->l |= chanSu;
  while (!(c->l & chanPe)) {
    p = *(c->p + c->ph);
    if (c->ps == 1)
      c->l |= chanPe;
    else {
      if (++c->ph == c->ps)
        c->ph = 0;
      if (c->ph == c->pt)
        c->l |= chanPe;
    }
    pthread_mutex_lock(&p->m);
    --p->c;
    if (p->w && !pthread_cond_signal(&p->r))
      pthread_mutex_unlock(&p->m);
    else
      dCpr(p);
  }
  while (!(c->l & chanGe)) {
    p = *(c->g + c->gh);
    if (c->gs == 1)
      c->l |= chanGe;
    else {
      if (++c->gh == c->gs)
        c->gh = 0;
      if (c->gh == c->gt)
        c->l |= chanGe;
    }
    pthread_mutex_lock(&p->m);
    --p->c;
    if (p->w && !pthread_cond_signal(&p->r))
      pthread_mutex_unlock(&p->m);
    else
      dCpr(p);
  }
  while (!(c->l & chanSe)) {
    p = *(c->s + c->sh);
    if (c->ss == 1)
      c->l |= chanSe;
    else {
      if (++c->sh == c->ss)
        c->sh = 0;
      if (c->sh == c->st)
        c->l |= chanSe;
    }
    pthread_mutex_lock(&p->m);
    --p->c;
    if (p->w && !pthread_cond_signal(&p->r))
      pthread_mutex_unlock(&p->m);
    else
      dCpr(p);
  }
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
  c->f(c->s);
  if (c->q && c->d)
    c->d(c->v);
  pthread_mutex_unlock(&c->m);
  pthread_mutex_destroy(&c->m);
  c->f(c);
}

unsigned int
chanPoll(
  long w
 ,unsigned int t
 ,chanPoll_t *a
){
  chan_t *c;
  cpr_t *p;
  cpr_t *m;
  cpr_t **qp;
  void *v;
  unsigned int i;
  unsigned int j;
  unsigned int k;
  struct timespec s;

  m = 0;
  if (!t || !a)
    goto exit0;
  j = 0; /* to apply chanOsTmo to the first non-chanPoNop entry */

  /* first pass through array looking for non-blocking exit */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {
  case chanPoNop:
    break;

  case chanPoSht:
    if (!(c = (a + i)->c))
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoGet:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->t & chanSsCanGet && (c->l & chanSu || c->l & chanGe || !(c->l & chanPe)))
      goto get;
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPut:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe)))
      goto put;
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPutWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe)))
      goto putWait;
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (!w)
    goto timeO;
  else if (w > 0) {
    static long nsps = 1000000000L;

    if (
#ifdef HAVE_CONDATTR_SETCLOCK
        clock_gettime(CLOCK_MONOTONIC, &s)
#else
        clock_gettime(CLOCK_REALTIME, &s)
#endif
    )
      goto exit0;
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

  /* second pass through array waiting at end of each line */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {
  case chanPoNop:
    break;

  case chanPoSht:
    if (!(c = (a + i)->c))
      break;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (!m && !(m = gCpr(c->a, c->f))) {
      pthread_mutex_unlock(&c->m);
      goto exit0;
    }
    if (!(c->l & chanSe)) {
      for (k = c->sh, qp = c->s + k;;) {
        if (*qp == m)
          goto fndSht2;
        if (++k == c->ss) {
          if (k == c->st)
            break;
          k = 0;
          qp = c->s;
        } else
          ++qp;
      }
      if (c->sh == c->st) {
        if (!(v = c->a(c->s, (c->ss + 1) * sizeof (*c->s)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->s = v;
        if ((k = c->ss - c->sh))
          for (qp = c->s + c->sh + k; k; --k, --qp)
            *qp = *(qp - 1);
        ++c->sh;
        ++c->ss;
      }
    }
    pthread_mutex_lock(&m->m);
    ++m->c;
    pthread_mutex_unlock(&m->m);
    *(c->s + c->st) = m;
    if (c->ss != 1 && ++c->st == c->ss)
      c->st = 0;
    c->l &= ~chanSe;
fndSht2:
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoGet:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->t & chanSsCanGet && (c->l & chanSu || c->l & chanGe || !(c->l & chanPe))) {
get:
      if (c->q)
        c->t = c->q(c->v, chanSoGet, c->l & (chanGe|chanPe), (a + i)->v);
      else {
        *((a + i)->v) = c->v;
        c->t = chanSsCanPut;
      }
      if (c->t & chanSsCanPut) while (!(c->l & chanPe)) {
        p = *(c->p + c->ph);
        if (c->ps == 1)
          c->l |= chanPe;
        else {
          if (++c->ph == c->ps)
            c->ph = 0;
          if (c->ph == c->pt)
            c->l |= chanPe;
        }
        pthread_mutex_lock(&p->m);
        --p->c;
        if (p->w && !pthread_cond_signal(&p->r)) {
          pthread_mutex_unlock(&p->m);
          break;
        }
        dCpr(p);
      }
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsGet;
      ++i;
      goto exit;
    }
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (!m && !(m = gCpr(c->a, c->f))) {
      pthread_mutex_unlock(&c->m);
      goto exit0;
    }
    if (!(c->l & chanGe)) {
      for (k = c->gh, qp = c->g + k;;) {
        if (*qp == m)
          goto fndGet2;
        if (++k == c->gs) {
          if (k == c->gt)
            break;
          k = 0;
          qp = c->g;
        } else
          ++qp;
      }
      if (c->gh == c->gt) {
        if (!(v = c->a(c->g, (c->gs + 1) * sizeof (*c->g)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->g = v;
        if ((k = c->gs - c->gh))
          for (qp = c->g + c->gh + k; k; --k, --qp)
            *qp = *(qp - 1);
        ++c->gh;
        ++c->gs;
      }
    }
    pthread_mutex_lock(&m->m);
    ++m->c;
    pthread_mutex_unlock(&m->m);
    *(c->g + c->gt) = m;
    if (c->gs != 1 && ++c->gt == c->gs)
      c->gt = 0;
    c->l &= ~chanGe;
fndGet2:
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPut:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
put:
      if (c->q)
        c->t = c->q(c->v, chanSoPut, c->l & (chanGe|chanPe), (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->t = chanSsCanGet;
      }
      if (c->t & chanSsCanGet) while (!(c->l & chanGe)) {
        p = *(c->g + c->gh);
        if (c->gs == 1)
          c->l |= chanGe;
        else {
          if (++c->gh == c->gs)
            c->gh = 0;
          if (c->gh == c->gt)
            c->l |= chanGe;
        }
        pthread_mutex_lock(&p->m);
        --p->c;
        if (p->w && !pthread_cond_signal(&p->r)) {
          pthread_mutex_unlock(&p->m);
          break;
        }
        dCpr(p);
      }
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsPut;
      ++i;
      goto exit;
    }
putQueue:
    if (!m && !(m = gCpr(c->a, c->f))) {
      pthread_mutex_unlock(&c->m);
      goto exit0;
    }
    if (!(c->l & chanPe)) {
      for (k = c->ph, qp = c->p + k;;) {
        if (*qp == m)
          goto fndPut2;
        if (++k == c->ps) {
          if (k == c->pt)
            break;
          k = 0;
          qp = c->p;
        } else
          ++qp;
      }
      if (c->ph == c->pt) {
        if (!(v = c->a(c->p, (c->ps + 1) * sizeof (*c->p)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->p = v;
        if ((k = c->ps - c->ph))
          for (qp = c->p + c->ph + k; k; --k, --qp)
            *qp = *(qp - 1);
        ++c->ph;
        ++c->ps;
      }
    }
    pthread_mutex_lock(&m->m);
    ++m->c;
    pthread_mutex_unlock(&m->m);
    *(c->p + c->pt) = m;
    if (c->ps != 1 && ++c->pt == c->ps)
      c->pt = 0;
    c->l &= ~chanPe;
fndPut2:
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPutWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->l & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsSht;
      ++i;
      goto exit;
    }
    if (c->t & chanSsCanPut && (c->l & chanPe || !(c->l & chanGe))) {
putWait:
      if (!m && !(m = gCpr(c->a, c->f))) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      if (!(c->l & chanPe) && c->ph == c->pt) {
        if (!(v = c->a(c->p, (c->ps + 1) * sizeof (*c->p)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->p = v;
        if ((k = c->ps - c->ph))
          for (qp = c->p + c->ph + k; k; --k, --qp)
            *qp = *(qp - 1);
        ++c->ph;
        ++c->ps;
      }
      pthread_mutex_lock(&m->m);
      ++m->c;
      pthread_mutex_unlock(&m->m);
      *(c->p + c->pt) = m;
      if (c->ps != 1 && ++c->pt == c->ps)
        c->pt = 0;
      c->l &= ~chanPe;
      if (c->q)
        c->t = c->q(c->v, chanSoPut, c->l & (chanGe|chanPe), (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->t = chanSsCanGet;
      }
      if (c->t & chanSsCanGet) while (!(c->l & chanGe)) {
        p = *(c->g + c->gh);
        if (c->gs == 1)
          c->l |= chanGe;
        else {
          if (++c->gh == c->gs)
            c->gh = 0;
          if (c->gh == c->gt)
            c->l |= chanGe;
        }
        pthread_mutex_lock(&p->m);
        --p->c;
        if (p->w && !pthread_cond_signal(&p->r)) {
          pthread_mutex_unlock(&p->m);
          break;
        }
        dCpr(p);
      }
      pthread_mutex_unlock(&c->m);
      pthread_mutex_lock(&m->m);
      if (m->c) {
        m->w = 1;
        if (w > 0) {
          if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
            m->w = 0;
            pthread_mutex_unlock(&m->m);
            (a + i)->s = chanOsPut;
            ++i;
            goto exit;
          }
        } else
          pthread_cond_wait(&m->r, &m->m);
      }
      m->w = 0;
      pthread_mutex_unlock(&m->m);
      pthread_mutex_lock(&c->m);
      /* since not taking an item, wake the next putter */
      if (c->t & chanSsCanPut) while (!(c->l & chanPe)) {
        p = *(c->p + c->ph);
        if (c->ps == 1)
          c->l |= chanPe;
        else {
          if (++c->ph == c->ps)
            c->ph = 0;
          if (c->ph == c->pt)
            c->l |= chanPe;
        }
        pthread_mutex_lock(&p->m);
        --p->c;
        if (p->w && !pthread_cond_signal(&p->r)) {
          pthread_mutex_unlock(&p->m);
          break;
        }
        dCpr(p);
      }
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsPutWait;
      ++i;
      goto exit;
    }
    goto putQueue;
    break;
  }
  if (!m)
    goto timeO;

  for (;;) {
    pthread_mutex_lock(&m->m);
    if (m->c) {
      m->w = 1;
      if (w > 0) {
        if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
          m->w = 0;
          pthread_mutex_unlock(&m->m);
          break;
        }
      } else
        pthread_cond_wait(&m->r, &m->m);
      m->w = 0;
    }
    pthread_mutex_unlock(&m->m);

    /* third pass through array looking for quick exit */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {
    case chanPoNop:
      break;

    case chanPoSht:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoGet:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->t & chanSsCanGet)
        goto get;
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPut:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (c->t & chanSsCanPut)
        goto put;
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPutWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (c->t & chanSsCanPut)
        goto putWait;
      pthread_mutex_unlock(&c->m);
      break;
    }

    /* fourth pass through array waiting at beginning of each line */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {
    case chanPoNop:
      break;

    case chanPoSht:
      if (!(c = (a + i)->c))
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (!(c->l & chanSe)) {
        for (k = c->sh, qp = c->s + k;;) {
          if (*qp == m)
            goto fndSht4;
          if (++k == c->ss) {
            if (k == c->st)
              break;
            k = 0;
            qp = c->s;
          } else
            ++qp;
        }
        if (c->sh == c->st) {
          if (!(v = c->a(c->s, (c->ss + 1) * sizeof (*c->s)))) {
            pthread_mutex_unlock(&c->m);
            goto exit0;
          }
          c->s = v;
          if ((k = c->ss - c->sh))
            for (qp = c->s + c->sh + k; k; --k, --qp)
              *qp = *(qp - 1);
          ++c->sh;
          ++c->ss;
        }
      }
      pthread_mutex_lock(&m->m);
      ++m->c;
      pthread_mutex_unlock(&m->m);
      if (c->ss == 1)
        *c->s = m;
      else {
        if (!c->sh)
          c->sh = c->ss;
        *(c->s + --c->sh) = m;
      }
      c->l &= ~chanSe;
fndSht4:
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoGet:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->t & chanSsCanGet)
        goto get;
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (!(c->l & chanGe)) {
        for (k = c->gh, qp = c->g + k;;) {
          if (*qp == m)
            goto fndGet4;
          if (++k == c->gs) {
            if (k == c->gt)
              break;
            k = 0;
            qp = c->g;
          } else
            ++qp;
        }
        if (c->gh == c->gt) {
          if (!(v = c->a(c->g, (c->gs + 1) * sizeof (*c->g)))) {
            pthread_mutex_unlock(&c->m);
            goto exit0;
          }
          c->g = v;
          if ((k = c->gs - c->gh))
            for (qp = c->g + c->gh + k; k; --k, --qp)
              *qp = *(qp - 1);
          ++c->gh;
          ++c->gs;
        }
      }
      pthread_mutex_lock(&m->m);
      ++m->c;
      pthread_mutex_unlock(&m->m);
      if (c->gs == 1)
        *c->g = m;
      else {
        if (!c->gh)
          c->gh = c->gs;
        *(c->g + --c->gh) = m;
      }
      c->l &= ~chanGe;
fndGet4:
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPut:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (c->t & chanSsCanPut)
        goto put;
      if (!(c->l & chanPe)) {
        for (k = c->ph, qp = c->p + k;;) {
          if (*qp == m)
            goto fndPut4;
          if (++k == c->ps) {
            if (k == c->pt)
              break;
            k = 0;
            qp = c->p;
          } else
            ++qp;
        }
        if (c->ph == c->pt) {
          if (!(v = c->a(c->p, (c->ps + 1) * sizeof (*c->p)))) {
            pthread_mutex_unlock(&c->m);
            goto exit0;
          }
          c->p = v;
          if ((k = c->ps - c->ph))
            for (qp = c->p + c->ph + k; k; --k, --qp)
              *qp = *(qp - 1);
          ++c->ph;
          ++c->ps;
        }
      }
      pthread_mutex_lock(&m->m);
      ++m->c;
      pthread_mutex_unlock(&m->m);
      if (c->ps == 1)
        *c->p = m;
      else {
        if (!c->ph)
          c->ph = c->ps;
        *(c->p + --c->ph) = m;
      }
      c->l &= ~chanPe;
fndPut4:
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPutWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->l & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsSht;
        ++i;
        goto exit;
      }
      if (c->t & chanSsCanPut)
        goto putWait;
      if (!(c->l & chanPe)) {
        for (k = c->ph, qp = c->p + k;;) {
          if (*qp == m)
            goto fndPutWait4;
          if (++k == c->ps) {
            if (k == c->pt)
              break;
            k = 0;
            qp = c->p;
          } else
            ++qp;
        }
        if (c->ph == c->pt) {
          if (!(v = c->a(c->p, (c->ps + 1) * sizeof (*c->p)))) {
            pthread_mutex_unlock(&c->m);
            goto exit0;
          }
          c->p = v;
          if ((k = c->ps - c->ph))
            for (qp = c->p + c->ph + k; k; --k, --qp)
              *qp = *(qp - 1);
          ++c->ph;
          ++c->ps;
        }
      }
      pthread_mutex_lock(&m->m);
      ++m->c;
      pthread_mutex_unlock(&m->m);
      if (c->ps == 1)
        *c->p = m;
      else {
        if (!c->ph)
          c->ph = c->ps;
        *(c->p + --c->ph) = m;
      }
      c->l &= ~chanPe;
fndPutWait4:
      pthread_mutex_unlock(&c->m);
      break;
    }
  }

timeO:
  if (j) {
    i = j;
    (a + (j - 1))->s = chanOsTmo;
    goto exit;
  }
exit0:
  i = 0;
exit:
  return (i);
}

chanOs_t
chanSht(
  long w
 ,chan_t *c
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = 0;
  p[0].o = chanPoSht;
  if (chanPoll(w, sizeof (p) / sizeof (p[0]), p))
    return p[0].s;
  return chanOsErr;
}

chanOs_t
chanGet(
  long w
 ,chan_t *c
 ,void **v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = v;
  p[0].o = chanPoGet;
  if (chanPoll(w, sizeof (p) / sizeof (p[0]), p))
    return p[0].s;
  return chanOsErr;
}

chanOs_t
chanPut(
  long w
 ,chan_t *c
 ,void *v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = &v;
  p[0].o = chanPoPut;
  if (chanPoll(w, sizeof (p) / sizeof (p[0]), p))
    return p[0].s;
  return chanOsErr;
}

chanOs_t
chanPutWait(
  long w
 ,chan_t *c
 ,void *v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = &v;
  p[0].o = chanPoPutWait;
  if (chanPoll(w, sizeof (p) / sizeof (p[0]), p))
    return p[0].s;
  return chanOsErr;
}
