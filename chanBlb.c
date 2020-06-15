/*
 * pthreadChannel - an implementation of CSP channels for pthreads
 * Copyright (C) 2019 G. David Butler <gdb@dbSystems.com>
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

#include <stdlib.h>     /* to support 0,0 to indicate realloc() and free() */
#include <unistd.h>     /* for read(), write() and close() */
#include <sys/socket.h> /* for shutdown() */
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"

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

struct chanBlbW {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  void (*d)(void *);
  chan_t *c;
  int s;
};

static void *
chanNfW(
  void *v
){
#define V ((struct chanBlbW *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    int i;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    for (l = 0, i = 1; l < m->l && (i = write(V->s, m->b + l, m->l - l)) > 0; l += i);
    pthread_cleanup_pop(1); /* V->f(m) */
    if (i <= 0)
      break;
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void *
chanNsW(
  void *v
){
#define V ((struct chanBlbW *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoGet;
  while (chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int l;
    int i;
    char b[16];

    i = sizeof (b) - 1;
    b[i] = ':';
    l = m->l;
    do {
      b[--i] = l % 10 + '0';
      l /= 10;
    } while (i && l);
    pthread_cleanup_push((void(*)(void*))V->f, m);
    if (!l
     && (l = sizeof (b) - i, i = write(V->s, &b[i], l)) > 0
     && (unsigned int)i == l)
      for (l = 0; l < m->l && (i = write(V->s, m->b + l, m->l - l)) > 0; l += i);
    else
      i = 0;
    pthread_cleanup_pop(1); /* V->f(m) */
    if (i <= 0
     || write(V->s, ",", 1) <= 0)
      break;
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

struct chanBlbR {
  void *(*a)(void *, unsigned long);
  void (*f)(void *);
  void (*d)(void *);
  chan_t *c;
  int s;
  unsigned int l;
};

static void *
chanNfR(
  void *v
){
#define V ((struct chanBlbR *)v)
  chanBlb_t *m;
  chanPoll_t p[1];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  while ((m = V->a(0, chanBlb_tSize(V->l)))) {
    void *t;
    int i;

    pthread_cleanup_push((void(*)(void*))V->f, m);
    i = read(V->s, m->b, V->l);
    pthread_cleanup_pop(0); /* V->f(m) */
    if (i <= 0)
      break;
    m->l = i;
    if ((t = V->a(m, chanBlb_tSize(m->l))))
      m = t;
    pthread_cleanup_push((void(*)(void*))V->f, m);
    i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* V->f(m) */
    if (!i)
      break;
  }
  V->f(m);
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void *
chanNsR(
  void *v
){
#define V ((struct chanBlbR *)v)
  chanBlb_t *m;
  chanPoll_t p[1];
  int r;
  int f;
  char b[16];

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  r = 0;
  while ((f = read(V->s, b + r, sizeof (b) - r)) > 0) {
    unsigned int l;
    int i;

    f += r;
    for (i = 0, l = 0; i < f && b[i] <= '9' && b[i] >= '0'; ++i)
      l = l * 10 + (b[i] - '0');
    if (i == f
     || b[i++] != ':'
     || (V->l && V->l < l)
     || !(m = V->a(0, chanBlb_tSize(l))))
      break;
    m->l = l;
    for (l = 0; l < m->l && i < f;)
      m->b[l++] = b[i++];
    for (r = 0; i < f;)
      b[r++] = b[i++];
    pthread_cleanup_push((void(*)(void*))V->f, m);
    for (; l < m->l && (i = read(V->s, m->b + l, m->l - l)) > 0; l += i);
    if (i > 0) {
      if ((r && b[--r] == ',')
       || (!r && (i = read(V->s, b, 1)) > 0 && b[0] == ','))
        i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      else
        i = 0;
    }
    pthread_cleanup_pop(0); /* V->f(m) */
    if (i <= 0) {
      V->f(m);
      break;
    }
  }
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void *
chanH1R(
  void *v
){
#define V ((struct chanBlbR *)v)
  chanBlb_t *m;
  void *tv;
  chanPoll_t p[1];
  unsigned int a;

  pthread_cleanup_push((void(*)(void*))V->f, v);
  pthread_cleanup_push((void(*)(void*))chanClose, V->c);
  pthread_cleanup_push((void(*)(void*))chanShut, V->c);
  pthread_cleanup_push((void(*)(void*))V->d, v);
  p[0].c = V->c;
  p[0].v = (void **)&m;
  p[0].o = chanPoPut;
  a = V->l ? V->l : 16384; /* 16k header limit */
  while ((m = V->a(0, chanBlb_tSize(a)))) {
    unsigned char *s1;
    unsigned char *s2;
    unsigned int i0;
    unsigned int i1;
    unsigned int i2;
    unsigned int cl;
    unsigned int tc;
    int i;

    cl = 0;
    tc = 0;
    for (i0 = 0; i0 < a; i0 += i) {
      pthread_cleanup_push((void(*)(void*))V->f, m);
      i = read(V->s, m->b + i0, a - i0);
      pthread_cleanup_pop(0); /* V->f(m) */
      if (i <= 0)
        goto bad;
next:
      for (i1 = (i0 > 32 ? 32 : i0) + i, s1 = m->b + i0 + i - i1; i1; --i1, ++s1)
        switch (*s1) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08:                       case 0x0b: case 0x0c:            case 0x0e: case 0x0f:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
                                                                                     case 0x7f:
          goto bad;
        case '\r': /* 0x0d */
          if (i1 < 4 || *(s1 + 1) != '\n')
            goto bad;
          switch (*(s1 + 2)) {
          case '\r': /* 0x0d */
            if (*(s1 + 3) == '\n') {
              i1 -= 4;
              s1 += 4;
              goto endHeader;
            }
          case 'c': case 'C':
            if (!cl && i1 > 18      /* \r\ncontent-length:0 */
             && (*(s1 +  9) == '-') /* easy disqual */
             && (*(s1 + 16) == ':') /* easy disqual */
/*           && (*(s1 +  0) == '\r') */
/*           && (*(s1 +  1) == '\n') */
/*           && (*(s1 +  2) == 'c' || *(s1 +  2) == 'C') */
             && (*(s1 +  3) == 'o' || *(s1 +  3) == 'O')
             && (*(s1 +  4) == 'n' || *(s1 +  4) == 'N')
             && (*(s1 +  5) == 't' || *(s1 +  5) == 'T')
             && (*(s1 +  6) == 'e' || *(s1 +  6) == 'E')
             && (*(s1 +  7) == 'n' || *(s1 +  7) == 'N')
             && (*(s1 +  8) == 't' || *(s1 +  8) == 'T')
/*           && (*(s1 +  9) == '-') */
             && (*(s1 + 10) == 'l' || *(s1 + 10) == 'L')
             && (*(s1 + 11) == 'e' || *(s1 + 11) == 'E')
             && (*(s1 + 12) == 'n' || *(s1 + 12) == 'N')
             && (*(s1 + 13) == 'g' || *(s1 + 13) == 'G')
             && (*(s1 + 14) == 't' || *(s1 + 14) == 'T')
             && (*(s1 + 15) == 'h' || *(s1 + 15) == 'H')
/*           && (*(s1 + 16) == ':') */
            ) {
              for (i2 = i1 - 17, s2 = s1 + 17; i2 && (*s2 == ' ' || *s2 == '\t'); --i2, ++s2);
              for (; i2 && *s2 <= '9' && *s2 >= '0'; --i2, ++s2)
                cl = cl * 10 + (*s2 - '0');
              i1 = i2 + 1;
              s1 = s2 - 1;
            }
            break;
          case 't': case 'T':
            if (!tc && i1 > 27      /* \r\ntransfer-encoding:chunked */
             && (*(s1 + 10) == '-') /* easy disqual */
             && (*(s1 + 19) == ':') /* easy disqual */
/*           && (*(s1 +  0) == '\r') */
/*           && (*(s1 +  1) == '\n') */
/*           && (*(s1 +  2) == 't' || *(s1 +  2) == 'T') */
             && (*(s1 +  3) == 'r' || *(s1 +  3) == 'R')
             && (*(s1 +  4) == 'a' || *(s1 +  4) == 'A')
             && (*(s1 +  5) == 'n' || *(s1 +  5) == 'N')
             && (*(s1 +  6) == 's' || *(s1 +  6) == 'S')
             && (*(s1 +  7) == 'f' || *(s1 +  7) == 'F')
             && (*(s1 +  8) == 'e' || *(s1 +  8) == 'E')
             && (*(s1 +  9) == 'r' || *(s1 +  9) == 'R')
/*           && (*(s1 + 10) == '-') */
             && (*(s1 + 11) == 'e' || *(s1 + 11) == 'E')
             && (*(s1 + 12) == 'n' || *(s1 + 12) == 'N')
             && (*(s1 + 13) == 'c' || *(s1 + 13) == 'C')
             && (*(s1 + 14) == 'o' || *(s1 + 14) == 'O')
             && (*(s1 + 15) == 'd' || *(s1 + 15) == 'D')
             && (*(s1 + 16) == 'i' || *(s1 + 16) == 'I')
             && (*(s1 + 17) == 'n' || *(s1 + 17) == 'N')
             && (*(s1 + 18) == 'g' || *(s1 + 18) == 'G')
/*           && (*(s1 + 19) == ':') */
            ) {
              for (i2 = i1 - 20, s2 = s1 + 20; i2 > 6; --i2, ++s2) {
                for (; i2 > 6 && *s2 != 'c' && *s2 != 'C' && *s2 != '\r' && *s2 != '\n'; --i2, ++s2);
                if (i2 > 6
                 && (*(s2 +  0) == 'c' || *(s2 +  0) == 'C')
                 && (*(s2 +  1) == 'h' || *(s2 +  1) == 'H')
                 && (*(s2 +  2) == 'u' || *(s2 +  2) == 'U')
                 && (*(s2 +  3) == 'n' || *(s2 +  3) == 'N')
                 && (*(s2 +  4) == 'k' || *(s2 +  4) == 'K')
                 && (*(s2 +  5) == 'e' || *(s2 +  5) == 'E')
                 && (*(s2 +  6) == 'd' || *(s2 +  6) == 'D')
                ) {
                  tc = 1;
                  break;
                }
              }
              i1 = i2 + 1;
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
    if (i <= 0 || i0 == a)
bad:
      break;
endHeader:
    m->l = i0 + i - i1;
    if (tc) {
      m->l += 2; /* last CRLF */
      cl = a;
      for (;;) {
        for (tc = 0, i2 = i1, s2 = s1; i2; --i2, ++s2)
          switch (*s2) {
          case '\r': case ';':
            for (; i2 > 1 && (*(s2 + 0) != '\r' || *(s2 + 1) != '\n'); --i2, ++s2);
            goto endCount;
          case '0': case '1': case '2': case '3': case '4': case '5':
          case '6': case '7': case '8': case '9':
            tc = tc * 16 + (*s2 - '0');
            break;
          case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            tc = tc * 16 + 10 + (*s2 - 'A');
            break;
          case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            tc = tc * 16 + 10 + (*s2 - 'a');
            break;
          default:
            goto bad;
          }
endCount:
        if (i2 < 4) {
          pthread_cleanup_push((void(*)(void*))V->f, m);
          i = read(V->s, m->b + i0, cl - i0);
          pthread_cleanup_pop(0); /* V->f(m) */
          if (i <= 0)
            goto bad;
          i0 += i;
          i1 += i;
          continue;
        }
        m->l += i1 - i2 + 2;
        i1 = i2 - 2;
        s1 = s2 + 2;
        if (!tc) {
          if (i1 < 2)
            goto bad;
          if (*(s1 + 0) == '\r' && *(s1 + 1) == '\n') {
            i1 -= 2;
            s1 += 2;
         /* m->l += 2; last CRLF */
          } else {
            for (;;) {
              for (i2 = i1, s2 = s1; i2 > 3 && (*(s2 + 0) != '\r' || *(s2 + 1) != '\n' || *(s2 + 2) != '\r' || *(s2 + 3) != '\n'); --i2, ++s2);
              if (i2 >= 4)
                break;
              pthread_cleanup_push((void(*)(void*))V->f, m);
              i = read(V->s, m->b + i0, cl - i0);
              pthread_cleanup_pop(0); /* V->f(m) */
              if (i <= 0)
                goto bad;
              i0 += i;
              i1 += i;
            }
            m->l += i1 - i2 + 2; /* 4 last CRLF */
            i1 = i2 - 4;
            s1 = s2 + 4;
          }
          break;
        }
        tc += 2;
        m->l += tc;
        if (i1 < tc) {
          if (m->l > a) {
            cl = m->l + 8192; /* 8k trailer limit */
            i2 = s1 - m->b;
            if (V->l || !(tv = V->a(m, chanBlb_tSize(cl))))
              goto bad;
            m = tv;
            s1 = m->b + i2;
          }
          pthread_cleanup_push((void(*)(void*))V->f, m);
          for (i0 += i; i0 < m->l && (i = read(V->s, m->b + i0, cl - i0)) > 0; i0 += i, i1 += i);
          pthread_cleanup_pop(0); /* V->f(m) */
          if (i <= 0)
            goto bad;
        }
        i1 -= tc;
        s1 += tc;
        if (*(s1 - 2) != '\r' || *(s1 - 1) != '\n')
          goto bad;
      }
      cl = 0;
    } else
      m->l += cl;
    if (i1 > cl) {
      chanBlb_t *m1;

      i1 -= cl;
      s1 += cl;
      i2 = i1;
      if ((m1 = V->a(0, chanBlb_tSize(a)))) {
        for (s2 = m1->b; i1; --i1, ++s1, ++s2)
          *s2 = *s1;
      }
      pthread_cleanup_push((void(*)(void*))V->f, m);
      pthread_cleanup_push((void(*)(void*))V->f, m1);
      i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
      pthread_cleanup_pop(0); /* V->f(m1) */
      pthread_cleanup_pop(0); /* V->f(m) */
      if (!i || !m1) {
        V->f(m1);
        goto bad;
      }
      m = m1;
      i0 = 0;
      i = i2;
      cl = 0;
      goto next;
    }
    if (i1 < cl) {
      if (m->l > a) {
        i2 = s1 - m->b;
        if (V->l || !(tv = V->a(m, chanBlb_tSize(m->l))))
          goto bad;
        m = tv;
        s1 = m->b + i2;
      }
      pthread_cleanup_push((void(*)(void*))V->f, m);
      for (i0 += i; i0 < m->l && (i = read(V->s, m->b + i0, m->l - i0)) > 0; i0 += i);
      pthread_cleanup_pop(0); /* V->f(m) */
      if (i <= 0)
        goto bad;
    }
    pthread_cleanup_push((void(*)(void*))V->f, m);
    i = chanPoll(-1, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut;
    pthread_cleanup_pop(0); /* V->f(m) */
    if (!i)
      goto bad;
  }
  V->f(m);
  pthread_cleanup_pop(1); /* V->d(v) */
  pthread_cleanup_pop(1); /* chanShut(V->c) */
  pthread_cleanup_pop(1); /* chanClose(V->c) */
  pthread_cleanup_pop(1); /* V->f(v) */
  return (0);
#undef V
}

static void
shutSockW(
  void *v
){
#define V ((struct chanBlbW *)v)
  shutdown(V->s, SHUT_WR);
#undef V
}

static void
shutSockR(
  void *v
){
#define V ((struct chanBlbR *)v)
  shutdown(V->s, SHUT_RD);
#undef V
}

int
chanSock(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *r
 ,chan_t *w
 ,int s
 ,chanBlbFrm_t m
 ,int g
){
  pthread_t t;

  if (((a || f) && (!a || !f))
   || (!r && !w)
   || s < 0
   || m < chanBlbFrmNf
   || m > chanBlbFrmH1
   || (m == chanBlbFrmNf && g <= 0)
   || (m == chanBlbFrmNs && g < 0)
   || (m == chanBlbFrmH1 && g < 0))
    goto error;
  if (!a) {
    a = realloc;
    f = free;
  }
  if (w) {
    struct chanBlbW *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = shutSockW;
    x->c = chanOpen(w);
    x->s = s;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsW
     :m == chanBlbFrmH1 ? chanNfW
     :chanNfW, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  if (r) {
    struct chanBlbR *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = shutSockR;
    x->c = chanOpen(r);
    x->s = s;
    x->l = g;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsR
     :m == chanBlbFrmH1 ? chanH1R
     :chanNfR, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  return (1);
error:
  return (0);
}

static void
closeFdW(
  void *v
){
#define V ((struct chanBlbW *)v)
  close(V->s);
#undef V
}

static void
closeFdR(
  void *v
){
#define V ((struct chanBlbR *)v)
  close(V->s);
#undef V
}

int
chanPipe(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,chan_t *r
 ,chan_t *w
 ,int d
 ,int e
 ,chanBlbFrm_t m
 ,int g
){
  pthread_t t;

  if (((a || f) && (!a || !f))
   || (!r && !w)
   || (r && d < 0)
   || (w && e < 0)
   || m < chanBlbFrmNf
   || m > chanBlbFrmH1
   || (m == chanBlbFrmNf && g <= 0)
   || (m == chanBlbFrmNs && g < 0)
   || (m == chanBlbFrmH1 && g < 0))
    goto error;
  if (!a) {
    a = realloc;
    f = free;
  }
  if (w) {
    struct chanBlbW *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = closeFdW;
    x->c = chanOpen(w);
    x->s = e;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsW
     :m == chanBlbFrmH1 ? chanNfW
     :chanNfW, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  if (r) {
    struct chanBlbR *x;

    if (!(x = a(0, sizeof (*x))))
      goto error;
    x->a = a;
    x->f = f;
    x->d = closeFdR;
    x->c = chanOpen(r);
    x->s = d;
    x->l = g;
    if (pthread_create(&t, 0
     ,m == chanBlbFrmNs ? chanNsR
     :m == chanBlbFrmH1 ? chanH1R
     :chanNfR, x)) {
      chanClose(x->c);
      f(x);
      goto error;
    }
    pthread_detach(t);
  }
  return (1);
error:
  return (0);
}
