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

#include "chan.h"
#include "chanStr.h"

extern void *(*ChanA)(void *, unsigned long);
extern void (*ChanF)(void *);

/* FIFO */

/* chan fifo store context */
struct chanFifoSc {
  void (*d)(void *); /* last close item callback */
  void **q;          /* circular store */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

void *
chanFifoSa(
  void (*d)(void *)
 ,unsigned int s
){
  struct chanFifoSc *c;

  if (!s)
    return (0);
  if (!(c = ChanA(0, sizeof (*c)))
   || !(c->q = ChanA(0, s * sizeof (*c->q)))) {
    ChanF(c);
    return (0);
  }
  c->d = d;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((struct chanFifoSc *)c)

void
chanFifoSd(
  void *c
 ,chanSs_t s
){
  if (s & chanSsCanGet) do {
    SC->d(SC->q[SC->h++]);
    if (SC->h == SC->s)
      SC->h = 0;
  } while (SC->h != SC->t);
  ChanF(SC->q);
  ChanF(c);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanFifoSi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  (void) w;
  if (o == chanSoPut) {
    SC->q[SC->t++] = *v;
    if (SC->t == SC->s)
      SC->t = 0;
    if (SC->t == SC->h)
      return (chanSsCanGet);
  } else {
    *v = SC->q[SC->h++];
    if (SC->h == SC->s)
      SC->h = 0;
    if (SC->h == SC->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
}

#undef SC

/* Latency Sensitive FIFO */

/* chan fifo store context */
struct chanFlsoSc {
  void (*d)(void *); /* last close item callback */
  void **q;          /* circular store */
  unsigned int m;    /* store max */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

void *
chanFlsoSa(
  void (*d)(void *)
 ,unsigned int m
 ,unsigned int s
){
  struct chanFlsoSc *c;

  if (!m || !s || m < s)
    return (0);
  if (!(c = ChanA(0, sizeof (*c)))
   || !(c->q = ChanA(0, m * sizeof (*c->q)))) {
    ChanF(c);
    return (0);
  }
  c->d = d;
  c->m = m;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((struct chanFlsoSc *)c)

void
chanFlsoSd(
  void *c
 ,chanSs_t s
){
  if (s & chanSsCanGet) do {
    SC->d(SC->q[SC->h++]);
    if (SC->h == SC->s)
      SC->h = 0;
  } while (SC->h != SC->t);
  ChanF(SC->q);
  ChanF(c);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanFlsoSi(
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
    SC->q[SC->t++] = *v;
    if (SC->t == SC->s)
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
    *v = SC->q[SC->h++];
    if (SC->h == SC->s)
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

/* LIFO */

/* chan lifo store context */
struct chanLifoSc {
  void (*d)(void *); /* last close item callback */
  void **q;          /* linear store */
  unsigned int s;    /* store size */
  unsigned int t;    /* store tail */
};

void *
chanLifoSa(
  void (*d)(void *)
 ,unsigned int s
){
  struct chanLifoSc *c;

  if (!s)
    return (0);
  if (!(c = ChanA(0, sizeof (*c)))
   || !(c->q = ChanA(0, s * sizeof (*c->q)))) {
    ChanF(c);
    return (0);
  }
  c->d = d;
  c->s = s;
  c->t = 0;
  return (c);
}

#define SC ((struct chanLifoSc *)c)

void
chanLifoSd(
  void *c
 ,chanSs_t s
){
  if (s & chanSsCanGet) do
    SC->d(SC->q[--SC->t]);
  while (SC->t);
  ChanF(SC->q);
  ChanF(c);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanLifoSi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  (void) w;
  if (o == chanSoPut) {
    SC->q[SC->t++] = *v;
    if (SC->t == SC->s)
      return (chanSsCanGet);
  } else {
    *v = SC->q[--SC->t];
    if (!SC->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
}

#undef SC
