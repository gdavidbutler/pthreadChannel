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
#include "chanStrFIFO.h"

struct chanStrFIFOc {
  void (*f)(void *); /* free routine */
  void (*d)(void *); /* item dequeue routine */
  void **q;          /* circular store */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

chanSs_t
chanStrFIFOa(
  chanStrFIFOc_t **c
 ,void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*d)(void *)
 ,unsigned int s
){
  if (!c)
    return (0);
  if (!a || !f || !s) {
    *c = 0;
    return (0);
  }
  f(a(0,1)); /* force exception here and now */
  if (!(*c = a(0, sizeof (**c)))
   || !((*c)->q = a(0, s * sizeof (*(*c)->q)))) {
    f(*c);
    *c = 0;
    return (0);
  }
  (*c)->f = f;
  (*c)->d = d;
  (*c)->s = s;
  (*c)->h = (*c)->t = 0;
  return (chanSsCanPut);
}

void
chanStrFIFOd(
  chanStrFIFOc_t *c
 ,chanSs_t s
){
  if (!c)
    return;
  if (s & chanSsCanGet && c->d)
    do {
      c->d(c->q[c->h]);
      if (++c->h == c->s)
        c->h = 0;
    } while (c->h != c->t);
  c->f(c->q);
  c->f(c);
}

chanSs_t
chanStrFIFOi(
  chanStrFIFOc_t *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  if (!c)
    return (0);
  if (o == chanSoPut) {
    c->q[c->t] = *v;
    if (++c->t == c->s)
      c->t = 0;
    if (c->t == c->h)
      return (chanSsCanGet);
  } else {
    *v = c->q[c->h];
    if (++c->h == c->s)
      c->h = 0;
    if (c->h == c->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
  (void) w;
}
