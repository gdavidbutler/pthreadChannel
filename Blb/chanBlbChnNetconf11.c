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
#include "chanBlbChnNetconf11.h"

void *
chanBlbChnNetconf11Egr(
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
      if (m->l)
        l = 2 + i + 1 + m->l + 4;
      else
        l = 4;
      if (!(t = v->realloc(0, l)))
        l = 0;
    }
    if (l) {
      unsigned char *s1;
      unsigned char *s2;

      pthread_cleanup_push((void(*)(void*))v->free, t);
      s2 = t;
      if (m->l) {
        *s2++ = '\n';
        *s2++ = '#';
        for (s1 = &b[o]; i; --i, ++s2, ++s1)
          *s2 = *s1;
        *s2++ = '\n';
        for (s1 = m->b, o = m->l; o; --o, ++s2, ++s1)
          *s2 = *s1;
      }
      *s2++ = '\n';
      *s2++ = '#';
      *s2++ = '#';
      *s2 = '\n';
      for (o = 0; o < l && (i = v->out(v->outCtx, t + o, l - o)) > 0; o += i);
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
chanBlbChnNetconf11Igr(
  struct chanBlbIgrCtx *v
){
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int l;
  unsigned int i0;
  unsigned int i1;
  int i;
  char b[16];

  l = v->frmCtx ? (long)v->frmCtx : 0;
  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  m = 0;
  i0 = 0;
  i1 = 0;
  while ((i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, b + i1, sizeof (b) - i1)
                     : v->inp(v->inpCtx, b + i1, sizeof (b) - i1)) > 0) {
    void *tv;
    unsigned int i2;
    unsigned int i3;
    unsigned int i4;

    i1 += i;
    while (i1 > 3) {
      i2 = 0;
      if (b[i2++] != '\n'
       || b[i2++] != '#')
        goto bad;
      for (i3 = 0; i2 < i1 && b[i2] <= '9' && b[i2] >= '0'; ++i2)
        i3 = i3 * 10 + (b[i2] - '0');
      if (i2 == i1) {
        if (i1 < (int)sizeof (b))
          break;
        goto bad;
      }
      if (!i3) {
        if (b[i2++] != '#'
         || b[i2++] != '\n'
         || chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPut)
          goto bad;
        m = 0;
        i0 = 0;
        for (; i2 < i1; ++i3, ++i2)
          b[i3] = b[i2];
        i1 = i3;
        continue;
      }
      i4 = i0 + i3;
      if (b[i2++] != '\n'
       || (l && l < i4)
       || !(tv = v->realloc(m, chanBlb_tSize(i4))))
        goto bad;
      m = tv;
      m->l = i4;
      for (; i3 && i2 < i1; --i3, ++i0, ++i2)
        *(m->b + i0) = b[i2];
      for (i4 = 0; i2 < i1; ++i4, ++i2)
        b[i4] = b[i2];
      i1 = i4;
      pthread_cleanup_push((void(*)(void*))v->free, m);
      for (; i3 && (i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, m->b + i0, i3)
                             : v->inp(v->inpCtx, m->b + i0, i3)) > 0; i3 -= i, i0 += i);
      pthread_cleanup_pop(0);
      if (i <= 0)
        goto bad;
    }
  }
bad:
  v->free(m);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
