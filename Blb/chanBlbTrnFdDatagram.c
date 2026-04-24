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

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include "chanBlbTrnFdDatagram.h"

struct ctx {
  void *(*ma)(void *, unsigned long);
  void (*mf)(void *);
  int *i4;
  int *i6;
  int *o4;
  int *o6;
  struct pollfd *pfd;
  unsigned int i4n;
  unsigned int i6n;
  unsigned int o4n;
  unsigned int o6n;
  unsigned int pfdn;
};
#define V ((struct ctx *)v)

static int
fdInArray(
  int fd
 ,const int *a
 ,unsigned int n
){
  unsigned int i;

  for (i = 0; i < n; ++i)
    if (a[i] == fd)
      return (1);
  return (0);
}

void *
chanBlbTrnFdDatagramCtx(
  void *(*ma)(void *, unsigned long)
 ,void (*mf)(void *)
){
  void *v;

  mf(ma(0, 1)); /* force exception here and now */
  if ((v = ma(0, sizeof (struct ctx)))) {
    memset(v, 0, sizeof (struct ctx));
    V->ma = ma;
    V->mf = mf;
  }
  return (v);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  unsigned int i;

  if (f4n) {
    V->i4 = (int *)V->ma(0, (unsigned long)f4n * sizeof (*V->i4));
    if (!V->i4) return (0);
    memcpy(V->i4, f4, f4n * sizeof (*V->i4));
    V->i4n = f4n;
  }
  if (f6n) {
    V->i6 = (int *)V->ma(0, (unsigned long)f6n * sizeof (*V->i6));
    if (!V->i6) goto error;
    memcpy(V->i6, f6, f6n * sizeof (*V->i6));
    V->i6n = f6n;
  }
  V->pfdn = f4n + f6n;
  if (!V->pfdn) goto error;
  V->pfd = (struct pollfd *)V->ma(0,
    (unsigned long)V->pfdn * sizeof (*V->pfd));
  if (!V->pfd) goto error;
  for (i = 0; i < f4n; ++i) {
    V->pfd[i].fd = f4[i];
    V->pfd[i].events = POLLIN;
    V->pfd[i].revents = 0;
  }
  for (i = 0; i < f6n; ++i) {
    V->pfd[f4n + i].fd = f6[i];
    V->pfd[f4n + i].events = POLLIN;
    V->pfd[f4n + i].revents = 0;
  }
  return (v);
error:
  V->mf(V->i4); V->i4 = 0; V->i4n = 0;
  V->mf(V->i6); V->i6 = 0; V->i6n = 0;
  V->pfdn = 0;
  return (0);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  socklen_t sl;
  unsigned int i;
  int r;

  if (!V->pfdn || l <= 1 + sizeof (struct sockaddr_storage))
    return (0);
retry:
  sl = sizeof (struct sockaddr_storage);
  if (V->pfdn == 1) {
    r = recvfrom(V->pfd[0].fd,
      b + 1 + sizeof (struct sockaddr_storage),
      l - 1 - sizeof (struct sockaddr_storage),
      0, (struct sockaddr *)&b[1], &sl);
  } else {
    for (i = 0; i < V->pfdn; ++i)
      V->pfd[i].revents = 0;
    if (poll(V->pfd, V->pfdn, -1) < 0) {
      if (errno == EBADF) return (0);
      goto retry;
    }
    r = 0;
    for (i = 0; i < V->pfdn; ++i) {
      /* any revents bit: POLLERR/POLLHUP/POLLNVAL wake us too
       * (e.g. ICMP after carrier drop); recvfrom consumes the
       * error so subsequent poll blocks normally again */
      if (V->pfd[i].revents) {
        sl = sizeof (struct sockaddr_storage);
        r = recvfrom(V->pfd[i].fd,
          b + 1 + sizeof (struct sockaddr_storage),
          l - 1 - sizeof (struct sockaddr_storage),
          0, (struct sockaddr *)&b[1], &sl);
        break;
      }
    }
  }
  /* r == 0 is a valid zero-length UDP datagram, not EOF */
  if (r < 0) {
    if (errno != EBADF && errno != ENOTSOCK)
      goto retry;
    return (0);
  }
  /* kernels may return sl > passed buffer on truncation; drop such packets */
  if (sl > sizeof (struct sockaddr_storage))
    return (0);
  b[0] = (unsigned char)sl;
  if (sl < sizeof (struct sockaddr_storage))
    memmove(b + 1 + sl, b + 1 + sizeof (struct sockaddr_storage), r);
  return (1 + sl + r);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  unsigned int i;

  for (i = 0; i < V->i4n; ++i)
    if (!fdInArray(V->i4[i], V->i4, i)
     && !fdInArray(V->i4[i], V->o4, V->o4n))
      close(V->i4[i]);
  for (i = 0; i < V->i6n; ++i)
    if (!fdInArray(V->i6[i], V->i6, i)
     && !fdInArray(V->i6[i], V->o6, V->o6n))
      close(V->i6[i]);
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,const int *f4
 ,const int *f6
 ,unsigned int f4n
 ,unsigned int f6n
){
  if (f4n) {
    V->o4 = (int *)V->ma(0, (unsigned long)f4n * sizeof (*V->o4));
    if (!V->o4) goto error;;
    memcpy(V->o4, f4, f4n * sizeof (*V->o4));
    V->o4n = f4n;
  }
  if (f6n) {
    V->o6 = (int *)V->ma(0, (unsigned long)f6n * sizeof (*V->o6));
    if (!V->o6) goto error;;
    memcpy(V->o6, f6, f6n * sizeof (*V->o6));
    V->o6n = f6n;
  }
  return (v);
error:
  V->mf(V->o4); V->o4 = 0; V->o4n = 0;
  V->mf(V->o6); V->o6 = 0; V->o6n = 0;
  return (0);
}

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  unsigned int i;

  if (l < 1
   || b[0] < sizeof (sa_family_t)
   || b[0] > sizeof (struct sockaddr_storage)
   || l <= 1U + b[0])
    return (0);
  /* no-fd-for-family and unknown family are drop-and-continue:
   * a stray blob must not tear down the egress */
  switch (((struct sockaddr *)&b[1])->sa_family) {
  case AF_INET:
    for (i = 0; i < V->o4n; ++i) {
      if (sendto(V->o4[i], b + 1 + b[0], l - 1 - b[0], 0,
            (struct sockaddr *)&b[1], b[0]) < 0
       && (errno == EBADF || errno == ENOTSOCK))
        return (0);
    }
    break;
  case AF_INET6:
    for (i = 0; i < V->o6n; ++i) {
      if (sendto(V->o6[i], b + 1 + b[0], l - 1 - b[0], 0,
            (struct sockaddr *)&b[1], b[0]) < 0
       && (errno == EBADF || errno == ENOTSOCK))
        return (0);
    }
    break;
  default:
    break;
  }
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  unsigned int i;

  for (i = 0; i < V->o4n; ++i)
    if (!fdInArray(V->o4[i], V->o4, i)
     && !fdInArray(V->o4[i], V->i4, V->i4n))
      close(V->o4[i]);
  for (i = 0; i < V->o6n; ++i)
    if (!fdInArray(V->o6[i], V->o6, i)
     && !fdInArray(V->o6[i], V->i6, V->i6n))
      close(V->o6[i]);
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  unsigned int i;

  for (i = 0; i < V->i4n; ++i)
    if (!fdInArray(V->i4[i], V->i4, i)
     && fdInArray(V->i4[i], V->o4, V->o4n))
      close(V->i4[i]);
  for (i = 0; i < V->i6n; ++i)
    if (!fdInArray(V->i6[i], V->i6, i)
     && fdInArray(V->i6[i], V->o6, V->o6n))
      close(V->i6[i]);
  V->mf(V->i4);
  V->mf(V->i6);
  V->mf(V->o4);
  V->mf(V->o6);
  V->mf(V->pfd);
  V->mf(v);
}

#undef V
