/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2025 G. David Butler <gdb@dbSystems.com>
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
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "chanBlbNetstring.h"

void *
chanBlbNetstringE(
  struct chanBlbEgrCtx *v
){
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned char *t;
    unsigned int o;
    unsigned int l;
    unsigned int i;
    unsigned char b[16];

    pthread_cleanup_push((void(*)(void*))v->free, m);
    for (l = m->l, o = sizeof (b); l && o; l /= 10)
      b[--o] = l % 10 + '0';
    if (!l) {
      i = sizeof (b) - o;
      l = i + 1 + m->l + 1;
      if (!(t = v->realloc(0, l)))
        l = 0;
    }
    if (l) {
      unsigned char *s1;
      unsigned char *s2;

      pthread_cleanup_push((void(*)(void*))v->free, t);
      for (s2 = t, s1 = &b[o]; i; --i, ++s2, ++s1)
        *s2 = *s1;
      *s2++ = ':';
      for (s1 = m->b, o = m->l; o; --o, ++s2, ++s1)
        *s2 = *s1;
      *s2 = ',';
      for (o = 0; o < l && (i = v->output(v->ctx, t + o, l - o)) > 0; o += i);
      pthread_cleanup_pop(1); /* v->free(t) */
    } else
      i = 0;
    pthread_cleanup_pop(1); /* v->free(m) */
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}

void *
chanBlbNetstringI(
  struct chanBlbIgrCtx *v
){
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int i0;
  unsigned int i;
  char b[16];

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i0 = 0;
  while ((i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, b + i0, sizeof (b) - i0)
                   : v->input(v->ctx, b + i0, sizeof (b) - i0)) > 0) {
    unsigned int i1;
    unsigned int i2;
    unsigned int i3;

    i0 += i;
    for (i1 = 0, i2 = 0; i1 < i0 && b[i1] <= '9' && b[i1] >= '0'; ++i1)
      i2 = i2 * 10 + (b[i1] - '0');
    if (i1 == i0 && i0 < (int)sizeof (b))
      continue;
    if (i1 == i0
     || b[i1++] != ':'
     || (v->arg && v->arg < i2)
     || !(m = v->realloc(0, chanBlb_tSize(i2))))
      break;
    m->l = i2;
    for (i3 = 0; i3 < i2 && i1 < i0;)
      *(m->b + i3++) = b[i1++];
    for (i2 = 0; i1 < i0;)
      b[i2++] = b[i1++];
    i0 = i2;
    pthread_cleanup_push((void(*)(void*))v->free, m);
    for (i2 = m->l; i3 < i2 && (i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, m->b + i3, i2 - i3)
                                         : v->input(v->ctx, m->b + i3, i2 - i3)) > 0; i3 += i);
    if (i > 0) {
      if ((i0 && b[--i0] == ',')
       || (!i0 && (i = v->input(v->ctx, b, 1)) == 1 && b[0] == ','))
        i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      else
        i = 0;
    } else
      i = 0;
    pthread_cleanup_pop(0); /* v->free(m) */
    if (!i) {
      v->free(m);
      break;
    }
  }
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
