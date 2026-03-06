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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include "chanBlbTrnFdDatagram.h"

struct ctx {
  int i4;
  int o4;
  int i6;
  int o6;
};
#define V ((struct ctx *)v)

void *
chanBlbTrnFdDatagramCtx(
  void
){
  void *v;

  if ((v = malloc(sizeof (struct ctx)))) {
    V->i4 = -1;
    V->o4 = -1;
    V->i6 = -1;
    V->o6 = -1;
  }
  return (v);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,int f4
 ,int f6
){
  V->i4 = f4;
  V->i6 = f6;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  socklen_t sl;
  int i;

  if (l <= 1 + sizeof (struct sockaddr_storage))
    return (0);
  sl = sizeof (struct sockaddr_storage);
  if (V->i6 < 0)
    i = recvfrom(V->i4, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
  else if (V->i4 < 0)
    i = recvfrom(V->i6, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
  else {
    struct pollfd p[2];

    if ((p[0].fd = V->i4) < 0)
      p[0].events = 0;
    else
      p[0].events = POLLIN;
    p[0].revents = 0;
    if ((p[1].fd = V->i6) < 0)
      p[1].events = 0;
    else
      p[1].events = POLLIN;
    p[1].revents = 0;
    if (poll(p, sizeof (p) / sizeof (p[0]), -1) < 0)
      return (0);
    if (p[0].revents == POLLIN)
      i = recvfrom(p[0].fd, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
    else if (p[1].revents == POLLIN)
      i = recvfrom(p[1].fd, b + 1 + sizeof (struct sockaddr_storage), l - 1 - sizeof (struct sockaddr_storage), 0, (struct sockaddr *)&b[1], &sl);
    else
      i = 0;
  }
  if (i <= 0)
    return (0);
  if ((b[0] = sl) < sizeof (struct sockaddr_storage))
    memmove(b + 1 + sl, b + 1 + sizeof (struct sockaddr_storage), i);
  return (1 + sl + i);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  if (V->i4 >= 0 && V->i4 != V->o4)
    close(V->i4);
  if (V->i6 >= 0 && V->i6 != V->o6)
    close(V->i6);
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,int f4
 ,int f6
){
  V->o4 = f4;
  V->o6 = f6;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  if (l < 1 || b[0] > sizeof (struct sockaddr_storage) || l <= 1 + b[0])
    return (0);
  switch (((struct sockaddr *)&b[1])->sa_family) {
  case AF_INET:
    if (V->o4 < 0 || sendto(V->o4, b + 1 + b[0], l - 1 - b[0], 0, (struct sockaddr *)&b[1], b[0]) < 0)
      return (0);
    break;
  case AF_INET6:
    if (V->o6 < 0 || sendto(V->o6, b + 1 + b[0], l - 1 - b[0], 0, (struct sockaddr *)&b[1], b[0]) < 0)
      return (0);
    break;
  default:
    return (0);
  }
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  if (V->o4 >= 0 && V->o4 != V->i4)
    close(V->o4);
  if (V->o6 >= 0 && V->o6 != V->i6)
    close(V->o6);
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  if (V->i4 >= 0 && V->i4 == V->o4)
    close(V->i4);
  if (V->i6 >= 0 && V->i6 == V->o6)
    close(V->i6);
  free(v);
}

#undef V
