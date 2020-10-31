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

#include "chan.h"
#include "chanFifo.h"

/* Static */

/* chan fifo store context */
struct chanFifoStSc {
  void (*f)(void *);
  void **q;          /* circular store */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

void *
chanFifoStSa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int s
){
  struct chanFifoStSc *c;

  if (!s)
    return (0);
  if (!(c = a(0, sizeof (*c)))
   || !(c->q = a(0, s * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->f = f;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((struct chanFifoStSc *)c)

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

/* Dynamic */

/* chan fifo store context */
struct chanFifoDySc {
  void (*f)(void *);
  void **q;          /* circular store */
  unsigned int m;    /* store max */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

void *
chanFifoDySa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int m
 ,unsigned int s
){
  struct chanFifoDySc *c;

  if (!m || !s || m < s)
    return (0);
  if (!(c = a(0, sizeof (*c)))
   || !(c->q = a(0, m * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->f = f;
  c->m = m;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((struct chanFifoDySc *)c)

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
