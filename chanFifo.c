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

/* chan fifo queue context */
struct chanFifoQc {
  void (*f)(void *); /* free */
  unsigned int s;    /* queue size */
  unsigned int h;    /* queue head */
  unsigned int t;    /* queue tail */
  void *q[1];        /* circular queue, must be last */
};

chanFifoQc_t *
chanFifoQa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,unsigned int s
){
  chanFifoQc_t *c;

  if (!a || !f || !s)
    return 0;
  if (!(c = a(0, sizeof(*c) + (s - 1) * sizeof(c->q[0]))))
    return c;
  c->f = f;
  c->s = s;
  c->h = c->t = 0;
  return c;
}

void
chanFifoQd(
  void *v
){
  ((struct chanFifoQc *)v)->f(v);
}

/* a queue is started in chanQsCanPut status */
chanQs_t
chanFifoQi(
  void *c
 ,chanQo_t o
 ,void **v
){
  if (o == chanQoPut) {
    ((chanFifoQc_t*)c)->q[((chanFifoQc_t*)c)->t] = *v;
    if (++((chanFifoQc_t*)c)->t == ((chanFifoQc_t*)c)->s)
      ((chanFifoQc_t*)c)->t = 0;
    if (((chanFifoQc_t*)c)->t == ((chanFifoQc_t*)c)->h)
      return chanQsCanGet;
  } else {
    *v = ((chanFifoQc_t*)c)->q[((chanFifoQc_t*)c)->h];
    if (++((chanFifoQc_t*)c)->h == ((chanFifoQc_t*)c)->s)
      ((chanFifoQc_t*)c)->h = 0;
    if (((chanFifoQc_t*)c)->h == ((chanFifoQc_t*)c)->t)
      return chanQsCanPut;
  }
  return chanQsCanGet | chanQsCanPut;
}
