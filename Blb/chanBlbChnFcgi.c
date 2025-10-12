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
#include "chanBlbChnFcgi.h"

void *
chanBlbChnFcgiEgr(
  struct chanBlbEgrCtx *v
){
  unsigned char *b;
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  if (!(b = v->realloc(0, 8 + 65535 + 255)))
    goto bad;
  *b = 1; /* FCGI_VERSION_1 */
  pthread_cleanup_push((void(*)(void*))v->free, b);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int i;

    pthread_cleanup_push((void(*)(void*))v->free, m);
    if (m->l > 2) { /* type, request1, request0 */
      unsigned int o1;
      unsigned int l1;

      *(b + 1) = *(m->b + 0);
      *(b + 2) = *(m->b + 1);
      *(b + 3) = *(m->b + 2);
      if (m->l == 3) {
        *(b + 4) = *(b + 5) = *(b + 6) = 0;
        for (o1 = 0, l1 = 8; o1 < l1 && (i = v->out(v->outCtx, b + o1, l1 - o1)) > 0; o1 += i);
      } else for (o1 = 3; o1 < m->l; o1 += l1) {
        unsigned char *s1;
        unsigned char *s2;
        unsigned int o2;
        unsigned int l2;

        if ((l1 = m->l - o1) > 65535)
          l1 = 65535;
        *(b + 4) = l1 >> 8 & 0xff;
        *(b + 5) = l1 >> 0 & 0xff;
        i = l1 % 8;
        *(b + 6) = i;
        for (s2 = b + 8, s1 = m->b + o1, l2 = l1; l2; ++s2, ++s1, --l2)
          *s2 = *s1;
        l2 = 8 + l1 + i;
        for (o2 = 0; o2 < l2 && (i = v->out(v->outCtx, b + o2, l2 - o2)) > 0; o2 += i);
      }
    } else
      i = 0;
    pthread_cleanup_pop(1); /* v->free(m) */
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* v->free(b) */
bad:
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}

void *
chanBlbChnFcgiIgr(
  struct chanBlbIgrCtx *v
){
  unsigned char *b;
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int l;
  unsigned int i0;
  unsigned int i;

  pthread_cleanup_push((void(*)(void*))v->fin, v);
  l = v->frmCtx ? (long)v->frmCtx : 8 + 65535 + 255;
  if (!(b = v->realloc(0, l)))
    goto bad;
  pthread_cleanup_push((void(*)(void*))v->free, b);
  p[0].c = v->chan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i0 = 0;
  while ((i = v->blb ? chanBlbIgrBlb(v->free, &v->blb, b + i0, l - i0)
                     : v->inp(v->inpCtx, b + i0, l - i0)) > 0) {
    unsigned char *s1;
    unsigned char *s2;
    unsigned int i1;
    unsigned int i2;

    i0 += i;
next:
    if (*b != 1) /* FCGI_VERSION_1 */
      break;
    if (i0 < 8
     || i0 < 8 + (i1 = *(b + 4) << 8 | *(b + 5)) + (i2 = *(b + 6)))
      continue;
    i0 -= 8 + i1 + i2;
    if (!(m = v->realloc(0, chanBlb_tSize(3 + i1))))
      break;
    pthread_cleanup_push((void(*)(void*))v->free, m);
    m->l = 3 + i1;
    *(m->b + 0) = *(b + 1);
    *(m->b + 1) = *(b + 2);
    *(m->b + 2) = *(b + 3);
    for (s2 = m->b + 3, s1 = b + 8; i1; ++s2, ++s1, --i1)
      *s2 = *s1;
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* v->free(m) */
    if (!i) {
      v->free(m);
      break;
    }
    if (i0) {
      s1 += i2;
      for (s2 = b, i2 = i0; i2; ++s2, ++s1, --i2)
        *s2 = *s1;
      goto next;
    }
  }
  pthread_cleanup_pop(1); /* v->free(b) */
bad:
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
