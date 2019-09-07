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

/* chanPoll rendezvous */
typedef struct {
  unsigned int c;    /* reference count */
  pthread_mutex_t m;
  pthread_cond_t r;
} cpr_t;

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
  int u;           /* channel is shutdown */
  pthread_mutex_t m;
};

chan_t *
chanAlloc(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chanQi_t q
 ,void *v
 ,chanQd_t d
){
  chan_t *c;

  c = 0;
  if (!a || !f || (q && !v))
    goto exit;
  if ((c = a(0, sizeof(*c)))) {
    c->r = c->w = 0;
    if (!(c->r = a(0, sizeof(*c->r)))
     || !(c->w = a(0, sizeof(*c->w)))
     || pthread_mutex_init(&c->m, 0)) {
      f(c->w);
      f(c->r);
      f(c);
      c = 0;
      goto exit;
    }
  } else
    goto exit;
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
  c->u = 0;
exit:
  return c;
}

void
chanShut(
  chan_t *c
){
  cpr_t *p;
  int r;

  r = pthread_mutex_lock(&c->m);
  assert(!r);
  c->u = 1;
  while (!c->we) {
    p = *(c->w + c->wh);
    if (++c->wh == c->ws)
      c->wh = 0;
    if (c->wh == c->wt)
      c->we = 1;
    r = pthread_mutex_lock(&p->m);
    assert(!r);
    assert(p->c);
    --p->c;
    if (!pthread_cond_signal(&p->r)) {
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
      continue;
    }
    if (!p->c) {
      r = pthread_cond_destroy(&p->r);
      assert(!r);
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
      r = pthread_mutex_destroy(&p->m);
      assert(!r);
      c->f(p);
    } else {
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
    }
  }
  while (!c->re) {
    p = *(c->r + c->rh);
    if (++c->rh == c->rs)
      c->rh = 0;
    if (c->rh == c->rt)
      c->re = 1;
    r = pthread_mutex_lock(&p->m);
    assert(!r);
    assert(p->c);
    --p->c;
    if (!pthread_cond_signal(&p->r)) {
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
      continue;
    }
    if (!p->c) {
      r = pthread_cond_destroy(&p->r);
      assert(!r);
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
      r = pthread_mutex_destroy(&p->m);
      assert(!r);
      c->f(p);
    } else {
      r = pthread_mutex_unlock(&p->m);
      assert(!r);
    }
  }
  r = pthread_mutex_unlock(&c->m);
  assert(!r);
}

void
chanFree(
  chan_t *c
){
  int r;

  r = pthread_mutex_lock(&c->m);
  assert(!r);
  if (!c->u) {
    void *v;

    r = pthread_mutex_unlock(&c->m);
    assert(!r);
    chanShut(c);
    while (chanRecv(1, c, &v));
    r = pthread_mutex_lock(&c->m);
    assert(!r);
  }
  assert(c->s == chanQsCanPut);
  c->f(c->r);
  c->f(c->w);
  if (c->q && c->d)
    c->d(c->v);
  r = pthread_mutex_unlock(&c->m);
  assert(!r);
  r = pthread_mutex_destroy(&c->m);
  assert(!r);
  c->f(c);
}

int
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
  int r;

  m = 0;
  /* first pass through array looking for no-wait exit */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNoop:
    break;

  case chanOpRecv:
    c = (a + i)->c;
    r = pthread_mutex_lock(&c->m);
    assert(!r);
    if (c->s & chanQsCanGet && (c->re || !c->we || c->u)) {
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
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      ++i;
      goto exit;
    }
    r = pthread_mutex_unlock(&c->m);
    assert(!r);
    break;

  case chanOpSend:
    c = (a + i)->c;
    r = pthread_mutex_lock(&c->m);
    assert(!r);
    if (c->u) {
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }
    if (c->s & chanQsCanPut && (c->we || !c->re)) {
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
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      ++i;
      goto exit;
    }
    r = pthread_mutex_unlock(&c->m);
    assert(!r);
    break;

  case chanOpSendWait:
    break;
  }
  if (n)
    return 0;

  /* second pass through array waiting at end of each line */
  for (i = 0; i < t; ++i) switch ((a + i)->o) {

  case chanOpNoop:
    break;

  case chanOpRecv:
    c = (a + i)->c;
    r = pthread_mutex_lock(&c->m);
    assert(!r);
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
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      ++i;
      goto exit;
    }
    if (n || c->u) {
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }
    if (!m) {
      if (!(m = c->a(0, sizeof(*m)))
       || pthread_mutex_init(&m->m, 0)) {
        c->f(m);
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        return 0;
      }
      if (pthread_cond_init(&m->r, 0)) {
        r = pthread_mutex_destroy(&m->m);
        assert(!r);
        c->f(m);
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        return 0;
      }
      m->c = 0;
      r = pthread_mutex_lock(&m->m);
      assert(!r);
    }
    if (!c->re && c->rh == c->rt) {
      if (!(v = c->a(c->r, (c->rs + 1) * sizeof(*c->r)))) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        i = 0;
        goto exit;
      }
      c->r = v;
      memmove(c->r + c->rh + 1, c->r + c->rh, c->rs - c->rh);
      ++c->rh;
      ++c->rs;
    }
    *(c->r + c->rt) = m;
    if (++c->rt == c->rs)
      c->rt = 0;
    c->re = 0;
    ++m->c;
    assert(m->c);
    r = pthread_mutex_unlock(&c->m);
    assert(!r);
    break;

  case chanOpSend:
    c = (a + i)->c;
    r = pthread_mutex_lock(&c->m);
    assert(!r);
    if (c->u) {
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
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
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      ++i;
      goto exit;
    }
sendQueue:
    if (n) {
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }
    if (!m) {
      if (!(m = c->a(0, sizeof(*m)))
       || pthread_mutex_init(&m->m, 0)) {
        c->f(m);
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        return 0;
      }
      if (pthread_cond_init(&m->r, 0)) {
        r = pthread_mutex_destroy(&m->m);
        assert(!r);
        c->f(m);
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        return 0;
      }
      m->c = 0;
      r = pthread_mutex_lock(&m->m);
      assert(!r);
    }
    if (!c->we && c->wh == c->wt) {
      if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        i = 0;
        goto exit;
      }
      c->w = v;
      memmove(c->w + c->wh + 1, c->w + c->wh, c->ws - c->wh);
      ++c->wh;
      ++c->ws;
    }
    *(c->w + c->wt) = m;
    if (++c->wt == c->ws)
      c->wt = 0;
    c->we = 0;
    ++m->c;
    assert(m->c);
    r = pthread_mutex_unlock(&c->m);
    assert(!r);
    break;

  case chanOpSendWait:
    c = (a + i)->c;
    r = pthread_mutex_lock(&c->m);
    assert(!r);
    if (c->u) {
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }
    if (c->s & chanQsCanPut && (c->we || !c->re)) {
wait:
      if (!m) {
        if (!(m = c->a(0, sizeof(*m)))
         || pthread_mutex_init(&m->m, 0)) {
          c->f(m);
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          return 0;
        }
        if (pthread_cond_init(&m->r, 0)) {
          r = pthread_mutex_destroy(&m->m);
          assert(!r);
          c->f(m);
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          return 0;
        }
        m->c = 0;
        r = pthread_mutex_lock(&m->m);
        assert(!r);
      }
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          i = 0;
          goto exit;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, c->ws - c->wh);
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
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      r = pthread_cond_wait(&m->r, &m->m);
      assert(!r);
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->s & chanQsCanPut) while (!c->we) {
        p = *(c->w + c->wh);
        if (++c->wh == c->ws)
          c->wh = 0;
        if (c->wh == c->wt)
          c->we = 1;
        r = pthread_mutex_lock(&p->m);
        assert(!r);
        assert(p->c);
        --p->c;
        if (!pthread_cond_signal(&p->r)) {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          break;
        }
        if (!p->c) {
          r = pthread_cond_destroy(&p->r);
          assert(!r);
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
          r = pthread_mutex_destroy(&p->m);
          assert(!r);
          c->f(p);
        } else {
          r = pthread_mutex_unlock(&p->m);
          assert(!r);
        }
      }
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      ++i;
      goto exit;
    }
    goto sendQueue;
    break;
  }
  if (!m)
    return 0;

  /* while there are references */
  while (m->c) {
    /* wait */
    r = pthread_cond_wait(&m->r, &m->m);
    assert(!r);

    /* third pass through array looking for quick exit */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNoop:
      break;

    case chanOpRecv:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->s & chanQsCanGet)
        goto recv;
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;

    case chanOpSend:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->u) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        break;
      }
      if (c->s & chanQsCanPut)
        goto send;
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;

    case chanOpSendWait:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->u) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        break;
      }
      if (c->s & chanQsCanPut)
        goto wait;
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }

    /* fourth pass through array waiting at beginning of each line */
    for (i = 0; i < t; ++i) switch ((a + i)->o) {

    case chanOpNoop:
      break;

    case chanOpRecv:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->s & chanQsCanGet)
        goto recv;
      if (c->u) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        break;
      }
      if (!c->re && c->rh == c->rt) {
        if (!(v = c->a(c->r, (c->rs + 1) * sizeof(*c->r)))) {
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          i = 0;
          goto exit;
        }
        c->r = v;
        memmove(c->r + c->rh + 1, c->r + c->rh, c->rs - c->rh);
        ++c->rh;
        ++c->rs;
      }
      if (!c->rh)
        c->rh = c->rs;
      *(c->r + --c->rh) = m;
      c->re = 0;
      ++m->c;
      assert(m->c);
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;

    case chanOpSend:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->u) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        break;
      }
      if (c->s & chanQsCanPut)
        goto send;
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          i = 0;
          goto exit;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, c->ws - c->wh);
        ++c->wh;
        ++c->ws;
      }
      if (!c->wh)
        c->wh = c->ws;
      *(c->w + --c->wh) = m;
      c->we = 0;
      ++m->c;
      assert(m->c);
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;

    case chanOpSendWait:
      c = (a + i)->c;
      r = pthread_mutex_lock(&c->m);
      assert(!r);
      if (c->u) {
        r = pthread_mutex_unlock(&c->m);
        assert(!r);
        break;
      }
      if (c->s & chanQsCanPut)
        goto wait;
      if (!c->we && c->wh == c->wt) {
        if (!(v = c->a(c->w, (c->ws + 1) * sizeof(*c->w)))) {
          r = pthread_mutex_unlock(&c->m);
          assert(!r);
          i = 0;
          goto exit;
        }
        c->w = v;
        memmove(c->w + c->wh + 1, c->w + c->wh, c->ws - c->wh);
        ++c->wh;
        ++c->ws;
      }
      if (!c->wh)
        c->wh = c->ws;
      *(c->w + --c->wh) = m;
      c->we = 0;
      ++m->c;
      assert(m->c);
      r = pthread_mutex_unlock(&c->m);
      assert(!r);
      break;
    }
  }
  i = 0;

exit:
  if (m) {
    if (m->c) {
      r = pthread_mutex_unlock(&m->m);
      assert(!r);
    } else {
      r = pthread_cond_destroy(&m->r);
      assert(!r);
      r = pthread_mutex_unlock(&m->m);
      assert(!r);
      r = pthread_mutex_destroy(&m->m);
      assert(!r);
      c->f(m);
    }
  }
  return i;
}

int
chanRecv(
  int n
 ,chan_t *c
 ,void **v
){
  chanPoll_t a;

  a.c = c;
  a.v = v;
  a.o = chanOpRecv;
  return chanPoll(n, 1, &a);
}

int
chanSend(
  int n
 ,chan_t *c
 ,void *v
){
  chanPoll_t a;

  a.c = c;
  a.v = &v;
  a.o = chanOpSend;
  return chanPoll(n, 1, &a);
}

int
chanSendWait(
  int n
 ,chan_t *c
 ,void *v
){
  chanPoll_t a;

  a.c = c;
  a.v = &v;
  a.o = chanOpSendWait;
  return chanPoll(n, 1, &a);
}
