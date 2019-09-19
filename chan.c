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

/* internal chanPoll rendezvous */
typedef struct {
  void (*f)(void *); /* free routine to free this */
  int t;             /* thread is active */
  unsigned int c;    /* reference count */
  pthread_mutex_t m;
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
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
){
  static pthread_once_t o = PTHREAD_ONCE_INIT;
  cpr_t *p;

  if (pthread_once(&o, cCpr))
    return (0);
  if (!(p = pthread_getspecific(Cpr))) {
    if (!(p = a(0, sizeof(*p)))
     || pthread_mutex_init(&p->m, 0)) {
      f(p);
      return (0);
    }
    if (pthread_cond_init(&p->r, 0)) {
      pthread_mutex_destroy(&p->m);
      f(p);
      return (0);
    }
    if (pthread_setspecific(Cpr, p)) {
      pthread_cond_destroy(&p->r);
      pthread_mutex_destroy(&p->m);
      f(p);
      return (0);
    }
    p->f = f;
    p->t = 1;
    p->c = 0;
  }
  pthread_mutex_lock(&p->m);
  return (p);
}

/* chan */
struct chan {
  void *(*a)(void *, unsigned long); /* realloc */
  void (*f)(void *);                 /* free */
  chanQi_t q;      /* queue implementation function */
  chanQd_t d;      /* queue done function */
  void *v;         /* if q, queue context else value */
  cpr_t **r;       /* reader circular queue */
  cpr_t **w;       /* writer circular queue */
  unsigned int rs; /* reader queue size */
  unsigned int rh; /* reader queue head */
  unsigned int rt; /* reader queue tail */
  int re;          /* reader queue empty, differentiate on rh==rt */
  unsigned int ws; /* writer queue size */
  unsigned int wh; /* writer queue head */
  unsigned int wt; /* writer queue tail */
  int we;          /* writer queue empty, differentiate on wh==wt */
  chanQs_t s;      /* queue status */
  unsigned int c;  /* open count */
  int u;           /* is shutdown */
  pthread_mutex_t m;
};

chan_t *
chanCreate(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chanQi_t q
 ,void *v
 ,chanQd_t d
){
  chan_t *c;

  if (!a || !f || (q && !v))
    return (0);
  if ((c = a(0, sizeof(*c)))) {
    c->r = c->w = 0;
    if (!(c->r = a(0, sizeof(*c->r)))
     || !(c->w = a(0, sizeof(*c->w)))
     || pthread_mutex_init(&c->m, 0)) {
      f(c->w);
      f(c->r);
      f(c);
      return (0);
    }
  } else
    return (0);
  c->a = a;
  c->f = f;
  c->rs = c->ws = 1;
  c->rh = c->rt = c->wh = c->wt = 0;
  c->re = c->we = 1;
  if ((c->q = q)) {
    c->v = v;
    c->d = d;
  }
  c->s = chanQsCanPut;
  c->c = 1;
  c->u = 0;
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
  if (c->u) {
    pthread_mutex_unlock(&c->m);
    return;
  }
  c->u = 1;
  while (!c->we) {
    p = *(c->w + c->wh);
    if (++c->wh == c->ws)
      c->wh = 0;
    if (c->wh == c->wt)
      c->we = 1;
    assert(p->c);
    pthread_mutex_lock(&p->m);
    --p->c;
    if (!pthread_cond_signal(&p->r)) {
      pthread_mutex_unlock(&p->m);
      continue;
    }
    dCpr(p);
  }
  while (!c->re) {
    p = *(c->r + c->rh);
    if (++c->rh == c->rs)
      c->rh = 0;
    if (c->rh == c->rt)
      c->re = 1;
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
  return (c->u);
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
  assert(c->s == chanQsCanPut);
  c->f(c->r);
  c->f(c->w);
  if (c->q && c->d)
    c->d(c->v);
  pthread_mutex_unlock(&c->m);
  pthread_mutex_destroy(&c->m);
  c->f(c);
}

unsigned int
chanPoll(
  int n
 ,unsigned int t
 ,chanPoll_t *a
){
  chan_t *c;
  cpr_t *p;
  cpr_t *m;
  void *v;
  unsigned int i;

  if (!t || !a)
    return (0);
  m = 0;
  if (t == 1)
    goto skipFirst;
  /* first pass through array looking for no-wait exit */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNoop:
    break;

  case chanOpRecv:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->s & chanQsCanGet && (c->re || !c->we || c->u))
      goto recv;
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpSend:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (!c->u && c->s & chanQsCanPut && (c->we || !c->re))
      goto send;
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpSendWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (!c->u && c->s & chanQsCanPut && (c->we || !c->re))
      goto sendWait;
    pthread_mutex_unlock(&c->m);
    break;
  }
  if (n)
    return (0);

skipFirst:
  /* second pass through array waiting at end of each line */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNoop:
    break;

  case chanOpRecv:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->s & chanQsCanGet && (c->re || !c->we || c->u)) {
recv:
      if (c->q)
        c->s = c->q(c->v, chanQoGet, (a + i)->v);
      else {
        *((a + i)->v) = c->v;
        c->s = chanQsCanPut;
      }
      if (c->s & chanQsCanPut) while (!c->we) {
        p = *(c->w + c->wh);
        if (++c->wh == c->ws)
          c->wh = 0;
        if (c->wh == c->wt)
          c->we = 1;
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
      ++i;
      goto exit;
    }
    if (n || c->u) {
      pthread_mutex_unlock(&c->m);
      break;
    }
    if (!m && !(m = gCpr(c->a, c->f))) {
      pthread_mutex_unlock(&c->m);
      return (0);
    }
    if (!c->re && c->rh == c->rt) {
      if (!(v = c->a(c->r, (c->rs + 1) * sizeof(*c->r)))) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      c->r = v;
      memmove(c->r + c->rh + 1, c->r + c->rh, (c->rs - c->rh) * sizeof(*c->r));
      ++c->rh;
      ++c->rs;
    }
    *(c->r + c->rt) = m;
    if (++c->rt == c->rs)
      c->rt = 0;
    c->re = 0;
    ++m->c;
    assert(m->c);
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpSend:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->u) {
      pthread_mutex_unlock(&c->m);
      break;
    }
    if (c->s & chanQsCanPut && (c->we || !c->re)) {
send:
      if (c->q)
        c->s = c->q(c->v, chanQoPut, (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->s = chanQsCanGet;
      }
      if (c->s & chanQsCanGet) while (!c->re) {
        p = *(c->r + c->rh);
        if (++c->rh == c->rs)
          c->rh = 0;
        if (c->rh == c->rt)
          c->re = 1;
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
      ++i;
      goto exit;
    }
sendQueue:
    if (n) {
      pthread_mutex_unlock(&c->m);
      break;
    }
    if (!m && !(m = gCpr(c->a, c->f))) {
      pthread_mutex_unlock(&c->m);
      return (0);
    }
    if (!c->we && c->wh == c->wt) {
      if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      c->w = v;
      memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof(*c->w));
      ++c->wh;
      ++c->ws;
    }
    *(c->w + c->wt) = m;
    if (++c->wt == c->ws)
      c->wt = 0;
    c->we = 0;
    ++m->c;
    assert(m->c);
    pthread_mutex_unlock(&c->m);
    break;

  case chanOpSendWait:
    if (!(c = (a + i)->c) || !(a + i)->v)
      break;
    pthread_mutex_lock(&c->m);
    if (c->u) {
      pthread_mutex_unlock(&c->m);
      break;
    }
    if (c->s & chanQsCanPut && (c->we || !c->re)) {
sendWait:
      /* after put wait on the write queue */
      if (!m && !(m = gCpr(c->a, c->f))) {
        pthread_mutex_unlock(&c->m);
        return (0);
      }
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof(*c->w));
        ++c->wh;
        ++c->ws;
      }
      *(c->w + c->wt) = m;
      if (++c->wt == c->ws)
        c->wt = 0;
      c->we = 0;
      ++m->c;
      assert(m->c);
      if (c->q)
        c->s = c->q(c->v, chanQoPut, (a + i)->v);
      else {
        c->v = *((a + i)->v);
        c->s = chanQsCanGet;
      }
      if (c->s & chanQsCanGet) while (!c->re) {
        p = *(c->r + c->rh);
        if (++c->rh == c->rs)
          c->rh = 0;
        if (c->rh == c->rt)
          c->re = 1;
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
      pthread_cond_wait(&m->r, &m->m);
      pthread_mutex_lock(&c->m);
      /* since not taking a message, wake the next writer */
      if (c->s & chanQsCanPut) while (!c->we) {
        p = *(c->w + c->wh);
        if (++c->wh == c->ws)
          c->wh = 0;
        if (c->wh == c->wt)
          c->we = 1;
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
      ++i;
      goto exit;
    }
    goto sendQueue;
    break;
  }
  if (!m)
    return (0);

  /* while there are references */
  while (m->c) {
    int isShut;

    pthread_cond_wait(&m->r, &m->m);
    if (t == 1)
      goto skipThird;
    isShut = 0;
    /* third pass through array looking for quick exit */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNoop:
      break;

    case chanOpRecv:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->s & chanQsCanGet)
        goto recv;
      else if (c->u)
        isShut = 1;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpSend:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->u)
        isShut = 1;
      else if (c->s & chanQsCanPut)
        goto send;
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpSendWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->u)
        isShut = 1;
      else if (c->s & chanQsCanPut)
        goto sendWait;
      pthread_mutex_unlock(&c->m);
      break;
    }
    if (isShut)
      goto exit0;

skipThird:
    /* fourth pass through array waiting at beginning of each line */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNoop:
      break;

    case chanOpRecv:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->s & chanQsCanGet)
        goto recv;
      if (c->u) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      if (!c->re && c->rh == c->rt) {
        if (!(v = c->a(c->r, (c->rs + 1) * sizeof(*c->r)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->r = v;
        memmove(c->r + c->rh + 1, c->r + c->rh, (c->rs - c->rh) * sizeof(*c->r));
        ++c->rh;
        ++c->rs;
      }
      if (!c->rh)
        c->rh = c->rs;
      *(c->r + --c->rh) = m;
      c->re = 0;
      ++m->c;
      assert(m->c);
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpSend:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->u) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      if (c->s & chanQsCanPut)
        goto send;
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof(*c->w));
        ++c->wh;
        ++c->ws;
      }
      if (!c->wh)
        c->wh = c->ws;
      *(c->w + --c->wh) = m;
      c->we = 0;
      ++m->c;
      assert(m->c);
      pthread_mutex_unlock(&c->m);
      break;

    case chanOpSendWait:
      if (!(c = (a + i)->c) || !(a + i)->v)
        break;
      pthread_mutex_lock(&c->m);
      if (c->u) {
        pthread_mutex_unlock(&c->m);
        goto exit0;
      }
      if (c->s & chanQsCanPut)
        goto sendWait;
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          pthread_mutex_unlock(&c->m);
          goto exit0;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, (c->ws - c->wh) * sizeof(*c->w));
        ++c->wh;
        ++c->ws;
      }
      if (!c->wh)
        c->wh = c->ws;
      *(c->w + --c->wh) = m;
      c->we = 0;
      ++m->c;
      assert(m->c);
      pthread_mutex_unlock(&c->m);
      break;
    }
  }
exit0:
  i = 0;
exit:
  if (m)
    pthread_mutex_unlock(&m->m);
  return (i);
}

unsigned int
chanRecv(
  int n
 ,chan_t *c
 ,void **v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = v;
  p[0].o = chanOpRecv;
  return (chanPoll(n, sizeof(p) / sizeof(p[0]), p));
}

unsigned int
chanSend(
  int n
 ,chan_t *c
 ,void *v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = &v;
  p[0].o = chanOpSend;
  return (chanPoll(n, sizeof(p) / sizeof(p[0]), p));
}

unsigned int
chanSendWait(
  int n
 ,chan_t *c
 ,void *v
){
  chanPoll_t p[1];

  p[0].c = c;
  p[0].v = &v;
  p[0].o = chanOpSendWait;
  return (chanPoll(n, sizeof(p) / sizeof(p[0]), p));
}
