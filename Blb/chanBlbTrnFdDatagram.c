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
#include "chanBlbTrnFdDatagram.h"

struct ctx {
  int inFd;
  int outFd;
};

void *
chanBlbTrnFdDatagramCtx(
  void
){
  struct ctx *c;

  if (!(c = malloc(sizeof (*c))))
    return (0);
  c->inFd = -1;
  c->outFd = -1;
  return (c);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,int f
){
  struct ctx *c = v;

  c->inFd = f;
  return (c);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  struct ctx *c = v;
  struct sockaddr_storage a;
  socklen_t al;
  int i;

  if (l < sizeof (a) + 2)
    return (0);
  al = sizeof (a);
  if ((i = recvfrom(c->inFd, b + sizeof (a) + 1, l - sizeof (a) - 1, 0, (struct sockaddr *)&a, &al)) <= 0)
    return (0);
  b[0] = (unsigned char)al;
  memcpy(b + 1, &a, al);
  memmove(b + 1 + al, b + sizeof (a) + 1, i);
  return (1 + al + i);
}

void
chanBlbTrnFdDatagramInputClose(
  void *v
){
  (void)v;
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,int f
){
  struct ctx *c = v;

  c->outFd = f;
  return (c);
}

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  struct ctx *c = v;
  struct sockaddr_storage a;
  unsigned int al;

  al = b[0];
  if (al > sizeof (a) || l < 1 + al)
    return (0);
  memcpy(&a, b + 1, al);
  if (sendto(c->outFd, b + 1 + al, l - 1 - al, 0, (struct sockaddr *)&a, al) < 0)
    return (0);
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  (void)v;
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  struct ctx *c = v;

  close(c->inFd);
  if (c->outFd != c->inFd)
    close(c->outFd);
  free(c);
}
