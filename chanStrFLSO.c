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

#define C ((struct chanStrFLSOc *)c)

void
chanStrFLSOd(
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

chanSs_t
chanStrFLSOi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  unsigned int i;

  if (!c)
    return (0);
  if (o == chanSoPut) {
    if (C->t == C->h
     && (w & chanSwNoGet)
     && C->s > 2) {
      --C->s;
      C->h = C->t = 0;
    }
    C->q[C->t] = *v;
    if (++C->t == C->s)
      C->t = 0;
    if (C->t == C->h) {
      if (!(w & chanSwNoGet)
       && C->s < C->m) {
        for (i = C->s; i > C->t; --i)
          C->q[i] = C->q[i - 1];
        ++C->s;
        ++C->h;
      } else
        return (chanSsCanGet);
    }
  } else {
    if (C->t == C->h
     && !(w & chanSwNoPut)
     && C->s < C->m) {
      for (i = C->s; i > C->t; --i)
        C->q[i] = C->q[i - 1];
      ++C->s;
      ++C->h;
    }
    *v = C->q[C->h];
    if (++C->h == C->s)
      C->h = 0;
    if (C->h == C->t) {
      if ((w & chanSwNoPut)
       && C->s > 2) {
        --C->s;
        C->h = C->t = 0;
      }
      return (chanSsCanPut);
    }
  }
  return (chanSsCanGet | chanSsCanPut);
}

#undef C

chanSs_t
chanStrFLSOa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*d)(void *)
 ,void *x
 ,int (*w)(void *, chanSs_t)
 ,void **v
 ,va_list l
){
  struct chanStrFLSOc *c;
  unsigned int m;
  unsigned int s;

  if (!v)
    return (0);
  m = va_arg(l, unsigned int);
  s = va_arg(l, unsigned int);
  if (!a || !f || !s || m < s) {
    *v = 0;
    return (0);
  }
  if (!(c = a(0, sizeof (*c)))
   || !(c->q = a(0, m * sizeof (*c->q)))) {
    f(c);
    *v = 0;
    return (0);
  }
  c->f = f;
  c->d = d;
  c->m = m;
  c->s = s;
  c->h = c->t = 0;
  *v = c;
  return (chanSsCanPut);
  (void)x;
  (void)w;
}
