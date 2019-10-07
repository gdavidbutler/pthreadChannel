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

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include "chan.h"

/*
 * Design note: A sleeping thread (on pthread_cond_wait) may be
 * cancelled, cleanup delegated to threads attempting wakeup
 */

void *(*ChanA)(void *, unsigned long) = realloc;
void (*ChanF)(void *) = free;

void
chanInit(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
){
  if (!a || !f)
    return;
  ChanA = a;
  ChanF = f;
}

/* internal chanPoll rendezvous */
typedef struct {
  int t;             /* thread is active */
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
    ChanF(p);
  }
}

static void
cdCpr(
  void *v
){
  pthread_mutex_lock(&((cpr_t *)v)->m);
  ((cpr_t *)v)->t = 0;
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
    if (pthread_setspecific(Cpr, p)) {
      pthread_cond_destroy(&p->r);
      pthread_condattr_destroy(&p->a);
      pthread_mutex_destroy(&p->m);
      ChanF(p);
      return (0);
    }
    p->t = 1;
    p->c = 0;
  }
  pthread_mutex_lock(&p->m);
  return (p);
}

/* chan */
struct chan {
  chanSi_t q;      /* store implementation function */
  chanSd_t d;      /* store done function */
  void *v;         /* if q, store context else value */
  cpr_t **r;       /* reader circular queue */
  cpr_t **w;       /* writer circular queue */
  chanSs_t s;      /* store status */
  unsigned int rs; /* reader queue size */
  unsigned int rh; /* reader queue head */
  unsigned int rt; /* reader queue tail */
  unsigned int ws; /* writer queue size */
  unsigned int wh; /* writer queue head */
  unsigned int wt; /* writer queue tail */
  unsigned int c;  /* open count */
  enum {
   chanSu = 1      /* is shutdown */
  ,chanRe = 2      /* reader queue empty, to differentiate rh==rt */
  ,chanWe = 4      /* writer queue empty, to differentiate wh==wt */
  } e;
  pthread_mutex_t m;
};

chan_t *
chanCreate(
  chanSi_t q
 ,void *v
 ,chanSd_t d
){
  chan_t *c;

  if ((c = ChanA(0, sizeof (*c)))) {
    c->r = c->w = 0;
    if (!(c->r = ChanA(0, sizeof (*c->r)))
     || !(c->w = ChanA(0, sizeof (*c->w)))
     || pthread_mutex_init(&c->m, 0)) {
      ChanF(c->w);
      ChanF(c->r);
      ChanF(c);
      return (0);
    }
  } else
    return (0);
  c->rs = c->ws = 1;
  c->rh = c->rt = c->wh = c->wt = 0;
  c->e = chanRe | chanWe;
  if ((c->q = q)) {
    c->v = v;
    c->d = d;
  }
  c->s = chanSsCanPut;
  c->c = 1;
  return (c);
}

void
chanOpen(
  chan_t *c
){
  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  ++c->c;
  pthread_mutex_unlock(&c->m);
}

void
chanShut(
  chan_t *c
){
  cpr_t *p;

  if (!c)
    return;
  pthread_mutex_lock(&c->m);
  if (c->e & chanSu) {
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->e |= chanSu;
  while (!(c->e & chanWe)) {
    p = *(c->w + c->wh);
    if (c->ws == 1)
      c->e |= chanWe;
    else {
      if (++c->wh == c->ws)
        c->wh = 0;
      if (c->wh == c->wt)
        c->e |= chanWe;
    }
    assert(p->c);
    pthread_mutex_lock(&p->m);
    --p->c;
    if (!pthread_cond_signal(&p->r)) {
      pthread_mutex_unlock(&p->m);
      continue;
    }
    dCpr(p);
  }
  while (!(c->e & chanRe)) {
    p = *(c->r + c->rh);
    if (c->rs == 1)
      c->e |= chanRe;
    else {
      if (++c->rh == c->rs)
        c->rh = 0;
      if (c->rh == c->rt)
        c->e |= chanRe;
    }
    assert(p->c);
    pthread_mutex_lock(&p->m);
    --p->c;
    if (!pthread_cond_signal(&p->r)) {
      pthread_mutex_unlock(&p->m);
      continue;
    }
    dCpr(p);
  }
  pthread_mutex_unlock(&c->m);
}

int
chanIsShut(
  chan_t *c
){
  return (c && (c->e & chanSu) == chanSu);
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
  assert(c->s == chanSsCanPut);
  ChanF(c->r);
  ChanF(c->w);
  if (c->q && c->d)
    c->d(c->v);
  pthread_mutex_unlock(&c->m);
  pthread_mutex_destroy(&c->m);
  ChanF(c);
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
  void *v;
  unsigned int i;
  unsigned int j;
  struct timespec s;

  m = 0;
  if (!t || !a)
    goto exit0;
  j = 0; /* to apply chanOsTmo to the first non-chanPoNop entry */

  /* first pass through array looking for non-blocking exit */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanPoNop:
    break;

  case chanPoGet:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->s & chanSsCanGet && (c->e & chanSu || c->e & chanRe || !(c->e & chanWe)))
      goto get;
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
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
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
      ++i;
      goto exit;
    }
    if (c->s & chanSsCanPut && (c->e & chanWe || !(c->e & chanRe)))
      goto put;
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPutWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    if (!j)
      j = i + 1;
    pthread_mutex_lock(&c->m);
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
      ++i;
      goto exit;
    }
    if (c->s & chanSsCanPut && (c->e & chanWe || !(c->e & chanRe)))
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

  case chanPoGet:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->s & chanSsCanGet && (c->e & chanSu || c->e & chanRe || !(c->e & chanWe))) {
get:
      if (c->q)
        c->s = c->q(c->v, chanSoGet, (a + i)->v);
      else {
        *((a + i)->v) = c->v;
        c->s = chanSsCanPut;
      }
      if (c->s & chanSsCanPut) while (!(c->e & chanWe)) {
        p = *(c->w + c->wh);
        if (c->ws == 1)
          c->e |= chanWe;
        else {
          if (++c->wh == c->ws)
            c->wh = 0;
          if (c->wh == c->wt)
            c->e |= chanWe;
        }
        assert(p->c);
        pthread_mutex_lock(&p->m);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
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
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
      ++i;
      goto exit;
    }
    if (!m && !(m = gCpr())) {
      pthread_mutex_unlock(&c->m);
      goto exit0;
    }
    if (!(c->e & chanRe) && c->rh == c->rt) {
      if (!(v = ChanA(c->r, (c->rs + 1) * sizeof (*c->r)))) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      c->r = v;
      memmove(c->r + c->rh + 1, c->r + c->rh, (c->rs - c->rh) * sizeof (*c->r));
      ++c->rh;
      ++c->rs;
    }
    *(c->r + c->rt) = m;
    if (c->rs != 1 && ++c->rt == c->rs)
      c->rt = 0;
    c->e &= ~chanRe;
    ++m->c;
    assert(m->c);
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPut:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
      ++i;
      goto exit;
    }
    if (c->s & chanSsCanPut && (c->e & chanWe || !(c->e & chanRe))) {
put:
      if (c->q)
        c->s = c->q(c->v, chanSoPut, (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->s = chanSsCanGet;
      }
      if (c->s & chanSsCanGet) while (!(c->e & chanRe)) {
        p = *(c->r + c->rh);
        if (c->rs == 1)
          c->e |= chanRe;
        else {
          if (++c->rh == c->rs)
            c->rh = 0;
          if (c->rh == c->rt)
            c->e |= chanRe;
        }
        assert(p->c);
        pthread_mutex_lock(&p->m);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
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
    if (!m && !(m = gCpr())) {
      pthread_mutex_unlock(&c->m);
      goto exit0;
    }
    if (!(c->e & chanWe) && c->wh == c->wt) {
      if (!(v = ChanA(c->w, (c->ws + 1) * sizeof (*c->w)))) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      c->w = v;
      memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof (*c->w));
      ++c->wh;
      ++c->ws;
    }
    *(c->w + c->wt) = m;
    if (c->ws != 1 && ++c->wt == c->ws)
      c->wt = 0;
    c->e &= ~chanWe;
    ++m->c;
    assert(m->c);
    pthread_mutex_unlock(&c->m);
    break;

  case chanPoPutWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->e & chanSu) {
      pthread_mutex_unlock(&c->m);
      (a + i)->s = chanOsShut;
      ++i;
      goto exit;
    }
    if (c->s & chanSsCanPut && (c->e & chanWe || !(c->e & chanRe))) {
putWait:
      if (!m && !(m = gCpr())) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      if (!(c->e & chanWe) && c->wh == c->wt) {
        if (!(v = ChanA(c->w, (c->ws + 1) * sizeof (*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof (*c->w));
        ++c->wh;
        ++c->ws;
      }
      *(c->w + c->wt) = m;
      if (c->ws != 1 && ++c->wt == c->ws)
        c->wt = 0;
      c->e &= ~chanWe;
      ++m->c;
      assert(m->c);
      if (c->q)
        c->s = c->q(c->v, chanSoPut, (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->s = chanSsCanGet;
      }
      if (c->s & chanSsCanGet) while (!(c->e & chanRe)) {
        p = *(c->r + c->rh);
        if (c->rs == 1)
          c->e |= chanRe;
        else {
          if (++c->rh == c->rs)
            c->rh = 0;
          if (c->rh == c->rt)
            c->e |= chanRe;
        }
        assert(p->c);
        pthread_mutex_lock(&p->m);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          pthread_mutex_unlock(&p->m);
          break;
        }
        dCpr(p);
      }
      pthread_mutex_unlock(&c->m);
      if (w > 0) {
        if (pthread_cond_timedwait(&m->r, &m->m, &s)) {
          (a + i)->s = chanOsPut;
          ++i;
          goto exit;
        }
      } else
        pthread_cond_wait(&m->r, &m->m);
      pthread_mutex_lock(&c->m);
      /* since not taking a message, wake the next writer */
      if (c->s & chanSsCanPut) while (!(c->e & chanWe)) {
        p = *(c->w + c->wh);
        if (c->ws == 1)
          c->e |= chanWe;
        else {
          if (++c->wh == c->ws)
            c->wh = 0;
          if (c->wh == c->wt)
            c->e |= chanWe;
        }
        assert(p->c);
        pthread_mutex_lock(&p->m);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
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
  while (m->c) {
    if (w > 0) {
      if (pthread_cond_timedwait(&m->r, &m->m, &s))
        goto timeO;
    } else
      pthread_cond_wait(&m->r, &m->m);
    /* third pass through array looking for quick exit */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanPoNop:
      break;

    case chanPoGet:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->s & chanSsCanGet)
        goto get;
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPut:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      if (c->s & chanSsCanPut)
        goto put;
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPutWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      if (c->s & chanSsCanPut)
        goto putWait;
      pthread_mutex_unlock(&c->m);
      break;
    }

    /* fourth pass through array waiting at beginning of each line */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanPoNop:
      break;

    case chanPoGet:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->s & chanSsCanGet)
        goto get;
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      if (!(c->e & chanRe) && c->rh == c->rt) {
        if (!(v = ChanA(c->r, (c->rs + 1) * sizeof (*c->r)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->r = v;
        memmove(c->r + c->rh + 1, c->r + c->rh, (c->rs - c->rh) * sizeof (*c->r));
        ++c->rh;
        ++c->rs;
      }
      if (c->rs == 1)
        *c->r = m;
      else {
        if (!c->rh)
          c->rh = c->rs;
        *(c->r + --c->rh) = m;
      }
      c->e &= ~chanRe;
      ++m->c;
      assert(m->c);
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPut:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      if (c->s & chanSsCanPut)
        goto put;
      if (!(c->e & chanWe) && c->wh == c->wt) {
        if (!(v = ChanA(c->w, (c->ws + 1) * sizeof (*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof (*c->w));
        ++c->wh;
        ++c->ws;
      }
      if (c->ws == 1)
        *c->w = m;
      else {
        if (!c->wh)
          c->wh = c->ws;
        *(c->w + --c->wh) = m;
      }
      c->e &= ~chanWe;
      ++m->c;
      assert(m->c);
      pthread_mutex_unlock(&c->m);
      break;

    case chanPoPutWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->e & chanSu) {
        pthread_mutex_unlock(&c->m);
        (a + i)->s = chanOsShut;
        ++i;
        goto exit;
      }
      if (c->s & chanSsCanPut)
        goto putWait;
      if (!(c->e & chanWe) && c->wh == c->wt) {
        if (!(v = ChanA(c->w, (c->ws + 1) * sizeof (*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof (*c->w));
        ++c->wh;
        ++c->ws;
      }
      if (c->ws == 1)
        *c->w = m;
      else {
        if (!c->wh)
          c->wh = c->ws;
        *(c->w + --c->wh) = m;
      }
      c->e &= ~chanWe;
      ++m->c;
      assert(m->c);
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
  if (m)
    pthread_mutex_unlock(&m->m);
  return (i);
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
  return chanOsFlr;
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
  return chanOsFlr;
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
  return chanOsFlr;
}
