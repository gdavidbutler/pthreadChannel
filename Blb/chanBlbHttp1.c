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
#include "chanBlbHttp1.h"

void *
chanBlbHttp1I(
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
    goto nextHeaders;
  } else if (!(m = v->realloc(0, chanBlb_tSize(l))))
    goto bad;
  else
    i0 = l;
  for (;;) {
    chanBlb_t *m1;
    void *tv;
    unsigned char *s1;
    unsigned char *s2;
    unsigned int cl;
    unsigned int ch;
    unsigned int i2;
    unsigned int i3;

    for (; i1 < i0; i1 += i) {
      pthread_cleanup_push((void(*)(void*))v->free, m);
      i = v->inp(v->inpCtx, m->b + i1, i0 - i1);
      pthread_cleanup_pop(0); /* v->free(m) */
      if (!i)
        goto bad;
nextHeaders:
      cl = 0;
      ch = 0;
      for (i2 = (i1 > 27 ? 28 : i1) + i, s1 = m->b + i1 + i - i2; i2; --i2, ++s1)
        switch (*s1) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08:                       case 0x0b: case 0x0c:            case 0x0e: case 0x0f:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
                                                                                     case 0x7f:
          goto bad;
        case '\r': /* 0x0d */
          if (i2 < 4)
            break;
          if (*(s1 + 1) != '\n')
            goto bad;
          switch (*(s1 + 2)) {
          case '\r': /* 0x0d */
            if (*(s1 + 3) != '\n')
              goto bad;
            i2 -= 4;
            s1 += 4;
            goto endHeaders;
          case 'c': case 'C':
            if (!cl && i2 > 18      /* \r\ncontent-length:0 */
             && (*(s1 +  9) == '-') /* easy disqual */
             && (*(s1 + 16) == ':') /* easy disqual */
          /* && (*(s1 +  0) == '\r') */
          /* && (*(s1 +  1) == '\n') */
          /* && (*(s1 +  2) == 'C' || *(s1 +  2) == 'c') */
             && (*(s1 +  3) == 'o' || *(s1 +  3) == 'O')
             && (*(s1 +  4) == 'n' || *(s1 +  4) == 'N')
             && (*(s1 +  5) == 't' || *(s1 +  5) == 'T')
             && (*(s1 +  6) == 'e' || *(s1 +  6) == 'E')
             && (*(s1 +  7) == 'n' || *(s1 +  7) == 'N')
             && (*(s1 +  8) == 't' || *(s1 +  8) == 'T')
          /* && (*(s1 +  9) == '-') */
             && (*(s1 + 10) == 'L' || *(s1 + 10) == 'l')
             && (*(s1 + 11) == 'e' || *(s1 + 11) == 'E')
             && (*(s1 + 12) == 'n' || *(s1 + 12) == 'N')
             && (*(s1 + 13) == 'g' || *(s1 + 13) == 'G')
             && (*(s1 + 14) == 't' || *(s1 + 14) == 'T')
             && (*(s1 + 15) == 'h' || *(s1 + 15) == 'H')
          /* && (*(s1 + 16) == ':') */
            ) {
              for (i3 = i2 - 17, s2 = s1 + 17; i3 && (*s2 == ' ' || *s2 == '\t'); --i3, ++s2);
              for (; i3 && *s2 <= '9' && *s2 >= '0'; --i3, ++s2)
                cl = cl * 10 + (*s2 - '0');
              i2 = i3 + 1;
              s1 = s2 - 1;
            }
            break;
          case 't': case 'T':
            if (!ch && i2 > 27      /* \r\ntransfer-encoding:chunked */
             && (*(s1 + 10) == '-') /* easy disqual */
             && (*(s1 + 19) == ':') /* easy disqual */
          /* && (*(s1 +  0) == '\r') */
          /* && (*(s1 +  1) == '\n') */
          /* && (*(s1 +  2) == 'T' || *(s1 +  2) == 't') */
             && (*(s1 +  3) == 'r' || *(s1 +  3) == 'R')
             && (*(s1 +  4) == 'a' || *(s1 +  4) == 'A')
             && (*(s1 +  5) == 'n' || *(s1 +  5) == 'N')
             && (*(s1 +  6) == 's' || *(s1 +  6) == 'S')
             && (*(s1 +  7) == 'f' || *(s1 +  7) == 'F')
             && (*(s1 +  8) == 'e' || *(s1 +  8) == 'E')
             && (*(s1 +  9) == 'r' || *(s1 +  9) == 'R')
          /* && (*(s1 + 10) == '-') */
             && (*(s1 + 11) == 'E' || *(s1 + 11) == 'e')
             && (*(s1 + 12) == 'n' || *(s1 + 12) == 'N')
             && (*(s1 + 13) == 'c' || *(s1 + 13) == 'C')
             && (*(s1 + 14) == 'o' || *(s1 + 14) == 'O')
             && (*(s1 + 15) == 'd' || *(s1 + 15) == 'D')
             && (*(s1 + 16) == 'i' || *(s1 + 16) == 'I')
             && (*(s1 + 17) == 'n' || *(s1 + 17) == 'N')
             && (*(s1 + 18) == 'g' || *(s1 + 18) == 'G')
          /* && (*(s1 + 19) == ':') */
            ) {
              for (i3 = i2 - 20, s2 = s1 + 20; i3 > 6; --i3, ++s2) {
                for (; i3 > 6 && *s2 != 'c' && *s2 != 'C' && *s2 != '\r' && *s2 != '\n'; --i3, ++s2);
                if (i3 > 6
              /* && (*(s2 +  0) == 'c' || *(s2 +  0) == 'C') */
                 && (*(s2 +  1) == 'h' || *(s2 +  1) == 'H')
                 && (*(s2 +  2) == 'u' || *(s2 +  2) == 'U')
                 && (*(s2 +  3) == 'n' || *(s2 +  3) == 'N')
                 && (*(s2 +  4) == 'k' || *(s2 +  4) == 'K')
                 && (*(s2 +  5) == 'e' || *(s2 +  5) == 'E')
                 && (*(s2 +  6) == 'd' || *(s2 +  6) == 'D')
                ) {
                  ch = 1;
                  i3 -= 7;
                  s2 += 7;
                  break;
                }
              }
              i2 = i3 + 1;
              s1 = s2 - 1;
            }
            break;
          default:
            break;
          }
          break;
        default:
          break;
        }
    }
    if (!v->frmCtx) {
      i0 += l;
      if (!(tv = v->realloc(m, chanBlb_tSize(i0))))
        goto bad;
      m = tv;
      continue;
    }
    goto bad;
endHeaders:
    m->l = i1 + i - i2;
    i1 = m->l;
    s2 = m->b;
    if (i1 < 11
     || *s2++ == ' ')
      goto bad;
    for (i1 -= 1; i1 && *s2 != ' '; --i1, ++s2);
    if (i1 < 10
     || *s2++ != ' '
     || *s2++ != '/')
      goto bad;
    for (i1 -= 2; i1 && *s2 != ' '; --i1, ++s2);
    if (i1 < 9
     || *s2++ != ' '
     || *s2++ != 'H'
     || *s2++ != 'T'
     || *s2++ != 'T'
     || *s2++ != 'P'
     || *s2   != '/')
      goto bad;
    if (v->frmCtx) {
      if (cl > i2)
        goto bad;
      if (ch && !i2)
        goto bad;
      i0 = l;
    } else {
      if (cl > i2)
        i0 = cl;
      else
        i0 = l;
    }
    if (!(m1 = v->realloc(0, chanBlb_tSize(i0))))
      goto bad;
    for (i1 = i2, s2 = m1->b; i1; --i1, ++s2, ++s1)
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
    if (ch) {
      for (;;) {
        for (i1 = i2; ; i1 += i) {
          for (ch = 0, i2 = i1, s1 = m->b; i2; --i2, ++s1)
            switch (*s1) {
            case ';':
              for (; i2 && *s1 != '\r'; --i2, ++s1);
              /* FALLTHROUGH */
            case '\r':
              if (i2 < 2)
                break;
              if (*(s1 + 1) != '\n')
                goto bad;
              i2 -= 2;
              s1 += 2;
              goto sizeHeader;
            case '0': case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
              ch = ch * 16 + (*s1 - '0');
              break;
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
              ch = ch * 16 + 10 + (*s1 - 'A');
              break;
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
              ch = ch * 16 + 10 + (*s1 - 'a');
              break;
            default:
              goto bad;
            }
          if (i1 == i0)
            goto bad;
          pthread_cleanup_push((void(*)(void*))v->free, m);
          i = v->inp(v->inpCtx, m->b + i1, i0 - i1);
          pthread_cleanup_pop(0); /* v->free(m) */
          if (!i)
            goto bad;
        }
sizeHeader:
        if (!ch)
          break;
        ch += 2;
        m->l = ch + i1 - i2;
        if (v->frmCtx && m->l > (long)v->frmCtx)
          goto bad;
        i0 = l;
        if (!(m1 = v->realloc(0, chanBlb_tSize(i0))))
          goto bad;
        if (i2 > ch) {
          for (i1 = i2 - ch, s2 = m1->b, s1 = m->b + m->l; i1; --i1, ++s1, ++s2)
            *s2 = *s1;
          if ((tv = v->realloc(m, chanBlb_tSize(m->l))))
            m = tv;
        } else if (i2 < ch) {
          if (!(tv = v->realloc(m, chanBlb_tSize(m->l)))) {
            v->free(m1);
            goto bad;
          }
          m = tv;
          s1 = m->b + i1 - i2;
          pthread_cleanup_push((void(*)(void*))v->free, m);
          for (s1 = m->b + i1 - i2; i2 < ch && (i = v->inp(v->inpCtx, s1 + i2, ch - i2)) > 0; i2 += i);
          pthread_cleanup_pop(0); /* v->free(m) */
          if (!i) {
            v->free(m1);
            goto bad;
          }
        }
        i2 -= ch;
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
      }
      for (i1 -= i2, i = i2; ; ) {
        for (i2 = (i1 > 2 ? 3 : i1) + i, s1 = m->b + i1 + i - i2; i2; --i2, ++s1)
          switch (*s1) {
          case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
          case 0x08:                       case 0x0b: case 0x0c:            case 0x0e: case 0x0f:
          case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
          case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
                                                                                       case 0x7f:
            goto bad;
          case '\r': /* 0x0d */
            if (i2 < 4)
              break;
            if (*(s1 + 1) != '\n')
              goto bad;
            if (*(s1 + 2) != '\r')
              break;
            if (*(s1 + 3) != '\n')
              goto bad;
            i2 -= 4;
            s1 += 4;
            goto endTrailers;
          default:
            break;
          }
        i1 += i;
        if (i1 == i0) {
          if (!v->frmCtx) {
            i0 += l;
            if (!(tv = v->realloc(m, chanBlb_tSize(i0))))
              goto bad;
            m = tv;
          } else
            goto bad;
        }
        pthread_cleanup_push((void(*)(void*))v->free, m);
        i = v->inp(v->inpCtx, m->b + i1, i0 - i1);
        pthread_cleanup_pop(0); /* v->free(m) */
        if (!i)
          goto bad;
      }
endTrailers:
      cl = i1 + i - i2;
      i2 = i1 + i;
    }
    if (cl) {
      i0 = l;
      if (!(m1 = v->realloc(0, chanBlb_tSize(i0))))
        goto bad;
      if (i2 > cl) {
        for (i1 = i0, s2 = m1->b, s1 = m->b + cl; i1; --i1, ++s2, ++s1)
          *s2 = *s1;
        if ((tv = v->realloc(m, chanBlb_tSize(cl))))
          m = tv;
      } else if (i2 < cl) {
        pthread_cleanup_push((void(*)(void*))v->free, m);
        for (s1 = m->b; i2 < cl && (i = v->inp(v->inpCtx, s1 + i2, cl - i2)) > 0; i2 += i);
        pthread_cleanup_pop(0); /* v->free(m) */
        if (!i) {
          v->free(m1);
          goto bad;
        }
      }
      i2 -= cl;
      m->l = cl;
      cl = 0;
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
    }
    if ((i = i2))
      goto nextHeaders;
  }
bad:
  v->free(m);
  pthread_cleanup_pop(1); /* v->fin(v) */
  return (0);
}
