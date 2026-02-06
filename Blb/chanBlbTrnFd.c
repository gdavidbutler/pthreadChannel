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
#include <unistd.h>
#include "chanBlbTrnFd.h"

struct ctx {
  int i;
  int o;
};
#define V ((struct ctx *)v)

void *
chanBlbTrnFdCtx(
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
chanBlbTrnFdInputCtx(
  void *v
 ,int f
){
  V->i = f;
  return (v);
}

unsigned int
chanBlbTrnFdInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  int i;

  if ((i = read(V->i, b, l)) < 0)
    i = 0;
  return (i);
}

void
chanBlbTrnFdInputClose(
  void *v
){
  if (V->i >= 0 && V->i != V->o)
    close(V->i);
}

void *
chanBlbTrnFdOutputCtx(
  void *v
 ,int f
){
  V->o = f;
  return (v);
}

unsigned int
chanBlbTrnFdOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  int i;

  if ((i = write(V->o, b, l)) < 0)
    i = 0;
  return (i);
}

void
chanBlbTrnFdOutputClose(
  void *v
){
  if (V->o >= 0 && V->o != V->i)
    close(V->o);
}

void
chanBlbTrnFdFinalClose(
  void *v
){
  if (V->i == V->o)
    close(V->i);
  free(v);
}

#undef V
