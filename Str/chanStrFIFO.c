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

#include <stdarg.h>
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

#define C ((struct chanStrFIFOc *)c)

static void
chanStrFIFOd(
  void *c
 ,chanSs_t s
){
  if (!c)
    return;
  if (s & chanSsCanGet && C->d)
    do {
      C->d(C->q[C->h]);
      if (++C->h == C->s)
        C->h = 0;
    } while (C->h != C->t);
  C->f(C->q);
  C->f(c);
}

static chanSs_t
chanStrFIFOi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  if (!c)
    return (0);
  if (o == chanSoPut) {
    C->q[C->t] = *v;
    if (++C->t == C->s)
      C->t = 0;
    if (C->t == C->h)
      return (chanSsCanGet);
  } else {
    *v = C->q[C->h];
    if (++C->h == C->s)
      C->h = 0;
    if (C->h == C->t)
      return (chanSsCanPut);
  }
  return (chanSsCanGet | chanSsCanPut);
  (void) w;
}

#undef C

chanSs_t
chanStrFIFOa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*u)(void *)
 ,int (*w)(void *, chanSs_t)
 ,void *x
 ,chanSd_t *d
 ,chanSi_t *i
 ,void **v
 ,va_list l
){
  struct chanStrFIFOc *c;
  unsigned int s;

  if (!v)
    return (0);
  *v = 0;
  s = va_arg(l, unsigned int);
  if (!a || !f || !s)
    return (0);
  if (!(c = a(0, sizeof (*c)))
   || !(c->q = a(0, s * sizeof (*c->q)))) {
    f(c);
    return (0);
  }
  c->s = s;
  c->h = c->t = 0;
  c->f = f;
  c->d = u;
  *d = chanStrFIFOd;
  *i = chanStrFIFOi;
  *v = c;
  return (chanSsCanPut);
  (void)w; /* not needed */
  (void)x; /* not needed */
}
