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
#include "chan.h"
#include "chanFifo.h"

extern void *(*ChanA)(void *, unsigned long);
extern void (*ChanF)(void *);

/* chan fifo store context */
struct chanFifoSc {
  void **q;          /* circular store */
  unsigned int m;    /* store max */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

chanFifoSc_t *
chanFifoSa(
  unsigned int m
 ,unsigned int s
){
  chanFifoSc_t *c;

  if (!m || !s || m < s)
    return (0);
  if (!(c = ChanA(0, sizeof (*c)))
   || !(c->q = ChanA(0, s * sizeof (*c->q)))) {
    ChanF(c);
    return (0);
  }
  c->m = m;
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

#define SC ((chanFifoSc_t*)c)

void
chanFifoSd(
  void *c
){
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
  void *t;
  unsigned int i;

  if (o == chanSoPut) {
    if (SC->t == SC->h
     && SC->s > 2 && (w & chanSwNoGet)
     && (t = ChanA(SC->q, (SC->s - 1) * sizeof (*SC->q)))) {
      SC->q = t;
      --SC->s;
      SC->h = SC->t = 0;
    }
    SC->q[SC->t] = *v;
    if (++SC->t == SC->s)
      SC->t = 0;
    if (SC->t == SC->h) {
      if (SC->s < SC->m && !(w & chanSwNoGet)
       && (t = ChanA(SC->q, (SC->s + 1) * sizeof (*SC->q)))) {
        SC->q = t;
        ++SC->h;
        for (i = SC->s; i > SC->t; --i)
          SC->q[i] = SC->q[i - 1];
        ++SC->s;
      } else
        return (chanSsCanGet);
    }
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
