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
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
  void *q[1];        /* circular store, must be last */
};

chanFifoSc_t *
chanFifoSa(
  unsigned int s
){
  chanFifoSc_t *c;

  if (!s)
    return (0);
  if (!(c = ChanA(0, sizeof (*c) + (s - 1) * sizeof (c->q[0]))))
    return (c);
  c->s = s;
  c->h = c->t = 0;
  return (c);
}

void
chanFifoSd(
  void *v
){
  ChanF(v);
}

/* a store is started in chanSsCanPut status */
chanSs_t
chanFifoSi(
  void *c
 ,chanSo_t o
 ,void **v
){
  if (o == chanSoPut) {
    ((chanFifoSc_t*)c)->q[((chanFifoSc_t*)c)->t] = *v;
    if (++((chanFifoSc_t*)c)->t == ((chanFifoSc_t*)c)->s)
      ((chanFifoSc_t*)c)->t = 0;
    if (((chanFifoSc_t*)c)->t == ((chanFifoSc_t*)c)->h)
      return (chanSsCanGet);
  } else {
    *v = ((chanFifoSc_t*)c)->q[((chanFifoSc_t*)c)->h];
    if (++((chanFifoSc_t*)c)->h == ((chanFifoSc_t*)c)->s)
      ((chanFifoSc_t*)c)->h = 0;
    if (((chanFifoSc_t*)c)->h == ((chanFifoSc_t*)c)->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
}
