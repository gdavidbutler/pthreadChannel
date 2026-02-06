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
  int i;
  int o;
};
#define V ((struct ctx *)v)

void *
chanBlbTrnFdDatagramCtx(
  void
){
  void *v;

  if ((v = malloc(sizeof (struct ctx)))) {
    V->i = -1;
    V->o = -1;
  }
  return (v);
}

void *
chanBlbTrnFdDatagramInputCtx(
  void *v
 ,int f
){
  V->i = f;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  struct sockaddr_storage a;
  socklen_t al;
  int i;

  if (l < sizeof (a) + 2)
    return (0);
  al = sizeof (a);
  if ((i = recvfrom(V->i, b + sizeof (a) + 1, l - sizeof (a) - 1, 0, (struct sockaddr *)&a, &al)) <= 0)
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
  if (V->i >= 0 && V->i != V->o)
    close(V->i);
}

void *
chanBlbTrnFdDatagramOutputCtx(
  void *v
 ,int f
){
  V->o = f;
  return (v);
}

unsigned int
chanBlbTrnFdDatagramOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  struct sockaddr_storage a;
  unsigned int al;

  al = b[0];
  if (al > sizeof (a) || l < 1 + al)
    return (0);
  memcpy(&a, b + 1, al);
  if (sendto(V->o, b + 1 + al, l - 1 - al, 0, (struct sockaddr *)&a, al) < 0)
    return (0);
  return (l);
}

void
chanBlbTrnFdDatagramOutputClose(
  void *v
){
  if (V->o >= 0 && V->o != V->i)
    close(V->o);
}

void
chanBlbTrnFdDatagramFinalClose(
  void *v
){
  if (V->i == V->o)
    close(V->i);
  free(v);
}

#undef V
