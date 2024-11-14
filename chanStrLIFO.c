/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2024 G. David Butler <gdb@dbSystems.com>
 *
 * This file is part of pthreadChannel
 *
 * pthreadChannel is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pthreadChannel is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "chan.h"
#include "chanStrLIFO.h"

struct chanStrLIFOc {
  void (*f)(void *); /* free routine */
  void (*d)(void *); /* item dequeue routine */
  void **q;          /* circular store */
  unsigned int s;    /* store size */
  unsigned int t;    /* store tail */
};

chanStrLIFOc_t *
chanStrLIFOa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*d)(void *)
 ,unsigned int s
){
  chanStrLIFOc_t *c;

  if (!a || !f || !s)
    return (0);
  if (!(c = a(0, sizeof (*c)))
   || !(c->q = a(0, s * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->f = f;
  c->d = d;
  c->s = s;
  c->t = 0;
  return (c);
}

void
chanStrLIFOd(
  chanStrLIFOc_t *c
 ,chanSs_t s
){
  if (!c)
    return;
  if (s & chanSsCanGet && c->d)
    do {
      --c->t;
      c->d(c->q[c->t]);
    } while (c->t);
  c->f(c->q);
  c->f(c);
}

chanSs_t
chanStrLIFOi(
  chanStrLIFOc_t *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  if (!c)
    return (0);
  if (o == chanSoPut) {
    c->q[c->t] = *v;
    if (++c->t == c->s)
      return (chanSsCanGet);
  } else {
    *v = c->q[--c->t];
    if (!c->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
  (void) w;
}
