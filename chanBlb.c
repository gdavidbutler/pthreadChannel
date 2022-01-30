/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2020 G. David Butler <gdb@dbSystems.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"

extern void *(*ChanA)(void *, unsigned long);
extern void (*ChanF)(void *);

unsigned int
chanBlb_tSize(
  unsigned int l
){
  return (
     l > sizeof (chanBlb_t) - (unsigned long)&((chanBlb_t *)0)->b
   ? l + (unsigned long)&((chanBlb_t *)0)->b
   : sizeof (chanBlb_t)
  );
}

struct chanBlbD {
  pthread_mutex_t m; /* destructor state mutex */
  unsigned int s;    /* destructor state */
};

/**********************************************************/

struct chanBlbE {
  void (*d)(void *); /* destructor */
  struct chanBlbD *ds;
  chan_t *c;
  void *x;
  unsigned int (*xo)(void *, const void *, unsigned int);
  void (*xc)(void *);
  void *f;
  void (*fc)(void *);
};

static void *
chanNfE(
  void *v
){
#define V ((struct chanBlbE *)v)
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    unsigned int i;

    pthread_cleanup_push((void(*)(void*))ChanF, m);
    for (l = 0, i = 1; l < m->l && (i = V->xo(V->x, m->b + l, m->l - l)) > 0; l += i);
    pthread_cleanup_pop(1); /* ChanF(m) */
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static void *
chanNsE(
  void *v
){
#define V ((struct chanBlbE *)v)
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int o;
    unsigned int l;
    unsigned int i;
    char b[16];

    pthread_cleanup_push((void(*)(void*))ChanF, m);
    l = m->l;
    o = sizeof (b) - 1;
    b[o] = ':';
    do {
      b[--o] = l % 10 + '0';
      l /= 10;
    } while (o && l);
    if (!l) {
      for (l = sizeof (b) - o, i = 0; l && (i = V->xo(V->x, &b[o], l)) > 0; l -= i, o += i);
      if (i > 0)
        for (l = 0; l < m->l && (i = V->xo(V->x, m->b + l, m->l - l)) > 0; l += i);
    } else
      i = 0;
    pthread_cleanup_pop(1); /* ChanF(m) */
    if (!i
     || V->xo(V->x, ",", 1) != 1)
      break;
  }
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static void *
chanN0E(
  void *v
){
#define V ((struct chanBlbE *)v)
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    const unsigned char t[] = "]]>]]>";
    unsigned int l;
    unsigned int i;

    pthread_cleanup_push((void(*)(void*))ChanF, m);
    for (l = 0, i = 1; l < m->l && (i = V->xo(V->x, m->b + l, m->l - l)) > 0; l += i);
    pthread_cleanup_pop(1); /* ChanF(m) */
    if (!i)
      break;
    for (l = 0, i = 0; l < (sizeof (t) - 1) && (i = V->xo(V->x, t + l, (sizeof (t) - 1) - l)) > 0; l += i);
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static void *
chanN1E(
  void *v
){
#define V ((struct chanBlbE *)v)
  chanBlb_t *m;
  chanArr_t p[1];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    const unsigned char t[] = "\n##\n";
    unsigned int o;
    unsigned int l;
    unsigned int i;
    char b[16];

    pthread_cleanup_push((void(*)(void*))ChanF, m);
    if (!(l = m->l)) {
      i = 1;
      goto empty;
    }
    o = sizeof (b) - 1;
    b[o] = '\n';
    do {
      b[--o] = l % 10 + '0';
      l /= 10;
    } while (o > 2 && l);
    if (!l) {
      b[--o] = '#';
      b[--o] = '\n';
      for (l = sizeof (b) - o, i = 0; l && (i = V->xo(V->x, &b[o], l)) > 0; l -= i, o += i);
      if (i > 0)
        for (l = 0; l < m->l && (i = V->xo(V->x, m->b + l, m->l - l)) > 0; l += i);
    } else
      i = 0;
empty:;
    pthread_cleanup_pop(1); /* ChanF(m) */
    if (!i)
      break;
    for (l = 0, i = 0; l < (sizeof (t) - 1) && (i = V->xo(V->x, t + l, (sizeof (t) - 1) - l)) > 0; l += i);
    if (!i)
      break;
  }
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

/**********************************************************/

struct chanBlbI {
  void (*d)(void *); /* destructor */
  struct chanBlbD *ds;
  chan_t *c;
  chanBlb_t *b;
  unsigned int l;
  void *x;
  unsigned int (*xo)(void *, void *, unsigned int);
  void (*xc)(void *);
  void *f;
  void (*fc)(void *);
};

static void *
chanNfI(
  void *v
){
#define V ((struct chanBlbI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int i;

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  if (V->b) {
    m = V->b;
    V->b = 0;
    pthread_cleanup_push((void(*)(void*))ChanF, m);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i)
      goto bad;
  }
  while ((m = ChanA(0, chanBlb_tSize(V->l)))) {
    void *t;

    pthread_cleanup_push((void(*)(void*))ChanF, m);
    i = V->xo(V->x, m->b, V->l);
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i)
      break;
    m->l = i;
    if ((t = ChanA(m, chanBlb_tSize(m->l))))
      m = t;
    pthread_cleanup_push((void(*)(void*))ChanF, m);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i)
      break;
  }
bad:
  ChanF(m);
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static unsigned int
chanBlbConsume(
  chanBlb_t **b
 ,void *d
 ,unsigned int l
){
  unsigned int i;
  unsigned int j;

  for (i = 0; i < l && i < (*b)->l; ++i)
    *((unsigned char *)d + i) = *((*b)->b + i);
  (*b)->l -= i;
  if ((*b)->l)
    for (j = 0; j < (*b)->l; ++j)
      *((*b)->b + j) = *((*b)->b + i + j);
  else {
    ChanF(*b);
    *b = 0;
  }
  return (i);
}

static void *
chanNsI(
  void *v
){
#define V ((struct chanBlbI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int i0;
  unsigned int i;
  char b[16];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i0 = 0;
  while ((i = V->b ? chanBlbConsume(&V->b, b + i0, sizeof (b) - i0)
                   :           V->xo(V->x, b + i0, sizeof (b) - i0)) > 0) {
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
     || (V->l && V->l < i2)
     || !(m = ChanA(0, chanBlb_tSize(i2))))
      break;
    m->l = i2;
    for (i3 = 0; i3 < i2 && i1 < i0;)
      *(m->b + i3++) = b[i1++];
    for (i2 = 0; i1 < i0;)
      b[i2++] = b[i1++];
    i0 = i2;
    pthread_cleanup_push((void(*)(void*))ChanF, m);
    for (i2 = m->l; i3 < i2 && (i = V->b ? chanBlbConsume(&V->b, m->b + i3, i2 - i3)
                                                   : V->xo(V->x, m->b + i3, i2 - i3)) > 0; i3 += i);
    if (i > 0) {
      if ((i0 && b[--i0] == ',')
       || (!i0 && (i = V->xo(V->x, b, 1)) == 1 && b[0] == ','))
        i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      else
        i = 0;
    } else
      i = 0;
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i) {
      ChanF(m);
      break;
    }
  }
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static void *
chanN0I(
  void *v
){
#define V ((struct chanBlbI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int bs;
  unsigned int i0;
  unsigned int i1;
  unsigned int i;

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  if (V->l)
    bs = V->l;
  else
    bs = 65536; /* when zero maxSize, balance data rate, io() call overhead and realloc() release policy */
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i1 = 0;
  if (V->b) {
    m = V->b;
    V->b = 0;
    i = i0 = m->l;
    goto next;
  } else if (!(m = ChanA(0, chanBlb_tSize(bs))))
    goto bad;
  else
    i0 = bs;
  for (;;) {
    chanBlb_t *m1;
    void *tv;
    unsigned char *s1;
    unsigned char *s2;
    unsigned int i2;

    for (; i1 < i0; i1 += i) {
      pthread_cleanup_push((void(*)(void*))ChanF, m);
      i = V->xo(V->x, m->b + i1, i0 - i1);
      pthread_cleanup_pop(0); /* ChanF(m) */
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
    if (!V->l) {
      i0 += bs;
      if (!(tv = ChanA(m, chanBlb_tSize(i0))))
        goto bad;
      m = tv;
      continue;
    }
    goto bad;
found:
    m->l = i1 + i - i2;
    i2 -= 6;
    s1 += 6;
    i0 = bs;
    if (!(m1 = ChanA(0, chanBlb_tSize(i0))))
      goto bad;
    for (i1 = i2, s2 = m1->b; i1; --i1, ++s1, ++s2)
      *s2 = *s1;
    if ((tv = ChanA(m, chanBlb_tSize(m->l))))
      m = tv;
    pthread_cleanup_push((void(*)(void*))ChanF, m);
    pthread_cleanup_push((void(*)(void*))ChanF, m1);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* ChanF(m1) */
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i) {
      ChanF(m1);
      goto bad;
    }
    m = m1;
    if ((i = i2))
      goto next;
  }
bad:
  ChanF(m);
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef X
#undef V
}

static void *
chanN1I(
  void *v
){
#define V ((struct chanBlbI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int i0;
  unsigned int i1;
  int i;
  char b[16];

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  m = 0;
  i0 = 0;
  i1 = 0;
  while ((i = V->b ? chanBlbConsume(&V->b, b + i1, sizeof (b) - i1)
                   :           V->xo(V->x, b + i1, sizeof (b) - i1)) > 0) {
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
       || (V->l && V->l < i4)
       || !(tv = ChanA(m, chanBlb_tSize(i4))))
        goto bad;
      m = tv;
      m->l = i4;
      for (; i3 && i2 < i1; --i3, ++i0, ++i2)
        *(m->b + i0) = b[i2];
      for (i4 = 0; i2 < i1; ++i4, ++i2)
        b[i4] = b[i2];
      i1 = i4;
      pthread_cleanup_push((void(*)(void*))ChanF, m);
      for (; i3 && (i = V->b ? chanBlbConsume(&V->b, m->b + i0, i3)
                             :           V->xo(V->x, m->b + i0, i3)) > 0; i3 -= i, i0 += i);
      pthread_cleanup_pop(0);
      if (i <= 0)
        goto bad;
    }
  }
bad:
  ChanF(m);
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef V
}

static void *
chanH1I(
  void *v
){
#define V ((struct chanBlbI *)v)
  chanBlb_t *m;
  chanArr_t p[1];
  unsigned int bs;
  unsigned int i0;
  unsigned int i1;
  unsigned int i;

  pthread_cleanup_push((void(*)(void*))ChanF, v);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  if (V->l)
    bs = V->l;
  else
    bs = 65536; /* when zero maxSize, balance data rate, io() call overhead and realloc() release policy */
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  i1 = 0;
  if (V->b) {
    m = V->b;
    V->b = 0;
    i = i0 = m->l;
    goto nextHeaders;
  } else if (!(m = ChanA(0, chanBlb_tSize(bs))))
    goto bad;
  else
    i0 = bs;
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
      pthread_cleanup_push((void(*)(void*))ChanF, m);
      i = V->xo(V->x, m->b + i1, i0 - i1);
      pthread_cleanup_pop(0); /* ChanF(m) */
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
    if (!V->l) {
      i0 += bs;
      if (!(tv = ChanA(m, chanBlb_tSize(i0))))
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
    if (V->l) {
      if (cl > i2)
        goto bad;
      if (ch && !i2)
        goto bad;
      i0 = bs;
    } else {
      if (cl > i2)
        i0 = cl;
      else
        i0 = bs;
    }
    if (!(m1 = ChanA(0, chanBlb_tSize(i0))))
      goto bad;
    for (i1 = i2, s2 = m1->b; i1; --i1, ++s2, ++s1)
      *s2 = *s1;
    if ((tv = ChanA(m, chanBlb_tSize(m->l))))
      m = tv;
    pthread_cleanup_push((void(*)(void*))ChanF, m);
    pthread_cleanup_push((void(*)(void*))ChanF, m1);
    i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* ChanF(m1) */
    pthread_cleanup_pop(0); /* ChanF(m) */
    if (!i) {
      ChanF(m1);
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
          pthread_cleanup_push((void(*)(void*))ChanF, m);
          i = V->xo(V->x, m->b + i1, i0 - i1);
          pthread_cleanup_pop(0); /* ChanF(m) */
          if (!i)
            goto bad;
        }
sizeHeader:
        if (!ch)
          break;
        ch += 2;
        m->l = ch + i1 - i2;
        if (V->l && m->l > V->l)
          goto bad;
        i0 = bs;
        if (!(m1 = ChanA(0, chanBlb_tSize(i0))))
          goto bad;
        if (i2 > ch) {
          for (i1 = i2 - ch, s2 = m1->b, s1 = m->b + m->l; i1; --i1, ++s1, ++s2)
            *s2 = *s1;
          if ((tv = ChanA(m, chanBlb_tSize(m->l))))
            m = tv;
        } else if (i2 < ch) {
          if (!(tv = ChanA(m, chanBlb_tSize(m->l)))) {
            ChanF(m1);
            goto bad;
          }
          m = tv;
          s1 = m->b + i1 - i2;
          pthread_cleanup_push((void(*)(void*))ChanF, m);
          for (s1 = m->b + i1 - i2; i2 < ch && (i = V->xo(V->x, s1 + i2, ch - i2)) > 0; i2 += i);
          pthread_cleanup_pop(0); /* ChanF(m) */
          if (!i) {
            ChanF(m1);
            goto bad;
          }
        }
        i2 -= ch;
        pthread_cleanup_push((void(*)(void*))ChanF, m);
        pthread_cleanup_push((void(*)(void*))ChanF, m1);
        i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
        pthread_cleanup_pop(0); /* ChanF(m1) */
        pthread_cleanup_pop(0); /* ChanF(m) */
        if (!i) {
          ChanF(m1);
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
          if (!V->l) {
            i0 += bs;
            if (!(tv = ChanA(m, chanBlb_tSize(i0))))
              goto bad;
            m = tv;
          } else
            goto bad;
        }
        pthread_cleanup_push((void(*)(void*))ChanF, m);
        i = V->xo(V->x, m->b + i1, i0 - i1);
        pthread_cleanup_pop(0); /* ChanF(m) */
        if (!i)
          goto bad;
      }
endTrailers:
      cl = i1 + i - i2;
      i2 = i1 + i;
    }
    if (cl) {
      i0 = bs;
      if (!(m1 = ChanA(0, chanBlb_tSize(i0))))
        goto bad;
      if (i2 > cl) {
        for (i1 = i0, s2 = m1->b, s1 = m->b + cl; i1; --i1, ++s2, ++s1)
          *s2 = *s1;
        if ((tv = ChanA(m, chanBlb_tSize(cl))))
          m = tv;
      } else if (i2 < cl) {
        pthread_cleanup_push((void(*)(void*))ChanF, m);
        for (s1 = m->b; i2 < cl && (i = V->xo(V->x, s1 + i2, cl - i2)) > 0; i2 += i);
        pthread_cleanup_pop(0); /* ChanF(m) */
        if (!i) {
          ChanF(m1);
          goto bad;
        }
      }
      i2 -= cl;
      m->l = cl;
      cl = 0;
      pthread_cleanup_push((void(*)(void*))ChanF, m);
      pthread_cleanup_push((void(*)(void*))ChanF, m1);
      i = chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      pthread_cleanup_pop(0); /* ChanF(m1) */
      pthread_cleanup_pop(0); /* ChanF(m) */
      if (!i) {
        ChanF(m1);
        goto bad;
      }
      m = m1;
    }
    if ((i = i2))
      goto nextHeaders;
  }
bad:
  ChanF(m);
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* ChanF(v) */
  return (0);
#undef X
#undef V
}

/**********************************************************/

static void
finE(
  void *v
){
#define V ((struct chanBlbE *)v)
  if (V->xc)
    V->xc(V->x);
  if (V->ds) {
    pthread_mutex_lock(&V->ds->m);
    if (!V->ds->s) {
      V->ds->s = 1;
      pthread_mutex_unlock(&V->ds->m);
      return;
    }
    pthread_mutex_unlock(&V->ds->m);
    pthread_mutex_destroy(&V->ds->m);
    ChanF(V->ds);
  }
  if (V->fc)
    V->fc(V->f);
#undef V
}

static void
finI(
  void *v
){
#define V ((struct chanBlbI *)v)
  ChanF(V->b);
  if (V->xc)
    V->xc(V->x);
  if (V->ds) {
    pthread_mutex_lock(&V->ds->m);
    if (!V->ds->s) {
      V->ds->s = 1;
      pthread_mutex_unlock(&V->ds->m);
      return;
    }
    pthread_mutex_unlock(&V->ds->m);
    pthread_mutex_destroy(&V->ds->m);
    ChanF(V->ds);
  }
  if (V->fc)
    V->fc(V->f);
#undef V
}

int
chanBlb(
  chan_t *i
 ,void *in
 ,unsigned int (*ino)(void *, void *, unsigned int)
 ,void (*inc)(void *)
 ,chan_t *e
 ,void *ot
 ,unsigned int (*oto)(void *, const void *, unsigned int)
 ,void (*otc)(void *)
 ,void *f
 ,void (*fc)(void *)
 ,chanBlbFrm_t m
 ,unsigned int g
 ,chanBlb_t *b
){
  pthread_t t;
  struct chanBlbD *d;

  d = 0;
  if ((!i && !e)
   || (i && !ino)
   || (e && !oto)
   || m < chanBlbFrmNf
   || m > chanBlbFrmH1
   || (m == chanBlbFrmNf && !g)
   || (m == chanBlbFrmNs && g > 0 && g < 3) /* 0:, is as short as it can get */
   || (m == chanBlbFrmN0 && g > 0 && g < 6) /* ]]>]]> is as short as it can get */
   || (m == chanBlbFrmN1 && g > 0 && g < 4) /* \n##\n is as short as it can get */
   || (m == chanBlbFrmH1 && g > 0 && g < 18) /* GET / HTTP/x.y\r\n\r\n is as short as it can get */
  )
    goto error;
  if (i && e && fc) {
    if (!(d = ChanA(0, sizeof (*d)))
     || pthread_mutex_init(&d->m, 0)) {
      ChanF(d);
      goto error;
    }
    d->s = 0;
  }
  if (e) {
    struct chanBlbE *x;

    if (!(x = ChanA(0, sizeof (*x))))
      goto error;
    x->d = finE;
    x->ds = d;
    x->c = chanOpen(e);
    x->x = ot;
    x->xo = oto;
    x->xc = otc;
    x->f = f;
    x->fc = fc;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsE
     :m == chanBlbFrmH1 ? chanNfE
     :m == chanBlbFrmN0 ? chanN0E
     :m == chanBlbFrmN1 ? chanN1E
     :chanNfE, x)) {
      chanClose(x->c);
      ChanF(x);
      goto error;
    }
    pthread_detach(t);
  }
  if (i) {
    struct chanBlbI *x;

    if (!(x = ChanA(0, sizeof (*x)))) {
      if (d) {
        pthread_mutex_lock(&d->m);
        if (!d->s) {
          d->s = 1;
          pthread_mutex_unlock(&d->m);
          d = 0;
        } else
          pthread_mutex_unlock(&d->m);
      }
      goto error;
    }
    x->d = finI;
    x->ds = d;
    x->c = chanOpen(i);
    x->x = in;
    x->xo = ino;
    x->xc = inc;
    x->f = f;
    x->fc = fc;
    x->l = g;
    x->b = b;
    b = 0;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsI
     :m == chanBlbFrmH1 ? chanH1I
     :m == chanBlbFrmN0 ? chanN0I
     :m == chanBlbFrmN1 ? chanN1I
     :chanNfI, x)) {
      if (d) {
        pthread_mutex_lock(&d->m);
        if (!d->s) {
          d->s = 1;
          pthread_mutex_unlock(&d->m);
          d = 0;
        } else
          pthread_mutex_unlock(&d->m);
      }
      chanClose(x->c);
      ChanF(x);
      goto error;
    }
    pthread_detach(t);
  }
  return (1);
error:
  if (d) {
    pthread_mutex_destroy(&d->m);
    ChanF(d);
  }
  chanShut(i);
  chanShut(e);
  if (otc)
    otc(ot);
  if (inc)
    inc(in);
  if (fc)
    fc(f);
  ChanF(b);
  return (0);
}
