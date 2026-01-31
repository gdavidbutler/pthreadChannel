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
#include "chanBlbChnVlq.h"

void *
chanBlbChnVlqEgr(
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
    b[(i = sizeof (b) - 1)] = (l = m->l) & 0x7f;
    while (l >>= 7)
      b[--i] = 0x80 | (--l & 0x7f);
    o = sizeof (b) - i;
    l = o + m->l;
    if (!(t = v->realloc(0, l)))
      l = 0;
    if (l) {
      unsigned char *s1;
      unsigned char *s2;

      pthread_cleanup_push((void(*)(void*))v->free, t);
      for (s2 = t, s1 = &b[i]; o; --o, ++s2, ++s1)
        *s2 = *s1;
      for (s1 = m->b, o = m->l; o; --o, ++s2, ++s1)
        *s2 = *s1;
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
chanBlbChnVlqIgr(
  struct chanBlbIgrCtx *v
){
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int l;
  unsigned int i0;
  unsigned int i;
  unsigned char b[16];

  l = v->frmCtx ? (long)v->frmCtx : 0;
  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i0 = 0;
  while ((i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, b + i0, sizeof (b) - i0)
                     : v->inp(v->inpCtx, b + i0, sizeof (b) - i0)) > 0) {
    unsigned int i1;
    unsigned int i2;
    unsigned int i3;

    i0 += i;
    for (i1 = 0, i2 = b[0] & 0x7f; i1 < i0 && b[i1] & 0x80; )
      if (!++i2 || i2 >> ((sizeof (i2) - 1) * 8 + 1))
        goto done;
      else
        i2 = (i2 << 7) | (b[++i1] & 0x7f);
    if (i1 >= i0 || b[i1] & 0x80)
      continue;
    ++i1;
    if ((l && l < i2)
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
                                           : v->inp(v->inpCtx, m->b + i3, i2 - i3)) > 0; i3 += i);
    if (i > 0)
      i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* v->free(m) */
    if (!i) {
      v->free(m);
      break;
    }
  }
done:
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
