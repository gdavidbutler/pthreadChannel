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
#include "chanStrFLSO.h"

struct chanStrFLSOc {
  void (*f)(void *); /* free routine */
  void (*d)(void *); /* item dequeue routine */
  void **q;          /* circular store */
  unsigned int m;    /* store max */
  unsigned int s;    /* store size */
  unsigned int h;    /* store head */
  unsigned int t;    /* store tail */
};

chanSs_t
chanStrFLSOa(
  chanStrFLSOc_t **c
 ,void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*d)(void *)
 ,unsigned int m
 ,unsigned int s
){
  if (!c)
    return (0);
  if (!a || !f || !s || m < s) {
    *c = 0;
    return (0);
  }
  if (!(*c = a(0, sizeof (**c)))
   || !((*c)->q = a(0, m * sizeof (*(*c)->q)))) {
    f(*c);
    *c = 0;
    return (0);
  }
  (*c)->f = f;
  (*c)->d = d;
  (*c)->m = m;
  (*c)->s = s;
  (*c)->h = (*c)->t = 0;
  return (chanSsCanPut);
}

void
chanStrFLSOd(
  chanStrFLSOc_t *c
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
chanStrFLSOi(
  chanStrFLSOc_t *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  unsigned int i;

  if (!c)
    return (0);
  if (o == chanSoPut) {
    if (c->t == c->h
     && (w & chanSwNoGet)
     && c->s > 2) {
      --c->s;
      c->h = c->t = 0;
    }
    c->q[c->t] = *v;
    if (++c->t == c->s)
      c->t = 0;
    if (c->t == c->h) {
      if (!(w & chanSwNoGet)
       && c->s < c->m) {
        for (i = c->s; i > c->t; --i)
          c->q[i] = c->q[i - 1];
        ++c->s;
        ++c->h;
      } else
        return (chanSsCanGet);
    }
  } else {
    if (c->t == c->h
     && !(w & chanSwNoPut)
     && c->s < c->m) {
      for (i = c->s; i > c->t; --i)
        c->q[i] = c->q[i - 1];
      ++c->s;
      ++c->h;
    }
    *v = c->q[c->h];
    if (++c->h == c->s)
      c->h = 0;
    if (c->h == c->t) {
      if ((w & chanSwNoPut)
       && c->s > 2) {
        --c->s;
        c->h = c->t = 0;
      }
      return (chanSsCanPut);
    }
  }
  return (chanSsCanGet | chanSsCanPut);
}
