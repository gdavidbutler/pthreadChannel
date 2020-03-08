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

#include <stdlib.h> /* to support chanFifoXxSa(0,0 ,...) to indicate realloc() and free() */
#include "chan.h"
#include "chanFifo.h"

/* chan fifo store context */
struct chanFifoStSc {
  void (*f)(void *);
  void **q;          /* circular store */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

chanFifoStSc_t *
chanFifoStSa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int s
){
  chanFifoStSc_t *c;

  if (((a || f) && (!a || !f)) || !s)
    return (0);
  if (a) {
    /* force exceptions here and now */
    if (!(c = a(0, sizeof (*c))))
      return (0);
    f(c);
    if (!(c = a(0, sizeof (*c))))
      return (0);
    c->f = f;
  } else {
    if (!(c = realloc(0, sizeof (*c))))
      return (0);
    a = realloc;
    c->f = f = free;
  }
  if (!(c->q = a(0, s * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((chanFifoStSc_t*)c)

void
chanFifoStSd(
  void *c
){
  SC->f(SC->q);
  SC->f(c);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanFifoStSi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  (void) w;
  if (o == chanSoPut) {
    SC->q[SC->t] = *v;
    if (++SC->t == SC->s)
      SC->t = 0;
    if (SC->t == SC->h)
      return (chanSsCanGet);
  } else {
    *v = SC->q[SC->h];
    if (++SC->h == SC->s)
      SC->h = 0;
    if (SC->h == SC->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
}

#undef SC

/* chan fifo store context */
struct chanFifoDySc {
  void (*f)(void *);
  void **q;          /* circular store */
  unsigned int m;    /* store max */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

chanFifoDySc_t *
chanFifoDySa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int m
 ,unsigned int s
){
  chanFifoDySc_t *c;

  if (((a || f) && (!a || !f)) || !m || !s || m < s)
    return (0);
  if (a) {
    /* force exceptions here and now */
    if (!(c = a(0, sizeof (*c))))
      return (0);
    f(c);
    if (!(c = a(0, sizeof (*c))))
      return (0);
    c->f = f;
  } else {
    if (!(c = realloc(0, sizeof (*c))))
      return (0);
    a = realloc;
    c->f = f = free;
  }
  if (!(c->q = a(0, m * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->m = m;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((chanFifoDySc_t*)c)

void
chanFifoDySd(
  void *c
){
  SC->f(SC->q);
  SC->f(c);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanFifoDySi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  unsigned int i;

  if (o == chanSoPut) {
    if (SC->t == SC->h
     && (w & chanSwNoGet)
     && SC->s > 2) {
      --SC->s;
      SC->h = SC->t = 0;
    }
    SC->q[SC->t] = *v;
    if (++SC->t == SC->s)
      SC->t = 0;
    if (SC->t == SC->h) {
      if (!(w & chanSwNoGet)
       && SC->s < SC->m) {
        for (i = SC->s; i > SC->t; --i)
          SC->q[i] = SC->q[i - 1];
        ++SC->s;
        ++SC->h;
      } else
        return (chanSsCanGet);
    }
  } else {
    if (SC->t == SC->h
     && !(w & chanSwNoPut)
     && SC->s < SC->m) {
      for (i = SC->s; i > SC->t; --i)
        SC->q[i] = SC->q[i - 1];
      ++SC->s;
      ++SC->h;
    }
    *v = SC->q[SC->h];
    if (++SC->h == SC->s)
      SC->h = 0;
    if (SC->h == SC->t) {
      if ((w & chanSwNoPut)
       && SC->s > 2) {
        --SC->s;
        SC->h = SC->t = 0;
      }
      return (chanSsCanPut);
    }
  }
  return (chanSsCanGet | chanSsCanPut);
}

#undef SC
