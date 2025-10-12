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
#include "chanBlbChnNetconf10.h"

void *
chanBlbChnNetconf10Egr(
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

    pthread_cleanup_push((void(*)(void*))v->free, m);
    l = m->l + 6;
    if (!(t = v->realloc(0, l)))
      l = 0;
    if (l) {
      unsigned char *s1;
      unsigned char *s2;

      pthread_cleanup_push((void(*)(void*))v->free, t);
      for (s2 = t, s1 = m->b, o = m->l; o; --o, ++s2, ++s1)
        *s2 = *s1;
      *s2++ = ']';
      *s2++ = ']';
      *s2++ = '>';
      *s2++ = ']';
      *s2++ = ']';
      *s2 = '>';
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
chanBlbChnNetconf10Igr(
  struct chanBlbIgrCtx *v
){
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int l;
  unsigned int i0;
  unsigned int i1;
  unsigned int i;

  l = v->frmCtx ? (long)v->frmCtx : 65536; /* when zero maxSize, balance data rate, io() call overhead and realloc() release policy */
  pthread_cleanup_push((void(*)(void*))v->fin, v);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i1 = 0;
  if (v->blb) {
    m = v->blb;
    v->blb = 0;
    i = i0 = m->l;
    goto next;
  } else if (!(m = v->realloc(0, chanBlb_tSize(l))))
    goto bad;
  else
    i0 = l;
  for (;;) {
    chanBlb_t *m1;
    void *tv;
    unsigned char *s1;
    unsigned char *s2;
    unsigned int i2;

    for (; i1 < i0; i1 += i) {
      pthread_cleanup_push((void(*)(void*))v->free, m);
      i = v->inp(v->inpCtx, m->b + i1, i0 - i1);
      pthread_cleanup_pop(0); /* v->free(m) */
      if (!i)
        goto bad;
next:
      for (i2 = (i1 > 4 ? 5 : i1) + i, s1 = m->b + i1 + i - i2; i2 > 5; --i2, ++s1)
        if (*(s1 + 0) == ']'
         && *(s1 + 1) == ']'
         && *(s1 + 2) == '>'
         && *(s1 + 3) == ']'
         && *(s1 + 4) == ']'
         && *(s1 + 5) == '>')
          goto found;
    }
    if (!v->frmCtx) {
      i0 += l;
      if (!(tv = v->realloc(m, chanBlb_tSize(i0))))
        goto bad;
      m = tv;
      continue;
    }
    goto bad;
found:
    m->l = i1 + i - i2;
    i2 -= 6;
    s1 += 6;
    i0 = l;
    if (!(m1 = v->realloc(0, chanBlb_tSize(i0))))
      goto bad;
    for (i1 = i2, s2 = m1->b; i1; --i1, ++s1, ++s2)
      *s2 = *s1;
    if ((tv = v->realloc(m, chanBlb_tSize(m->l))))
      m = tv;
    pthread_cleanup_push((void(*)(void*))v->free, m);
    pthread_cleanup_push((void(*)(void*))v->free, m1);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* v->free(m1) */
    pthread_cleanup_pop(0); /* v->free(m) */
    if (!i) {
      v->free(m1);
      goto bad;
    }
    m = m1;
    if ((i = i2))
      goto next;
  }
bad:
  v->free(m);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
