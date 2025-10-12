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

#include <unistd.h>
#include "chanBlbTrnFd.h"

void *
chanBlbTrnFdCtx(
  void
){
  return ((void *)(long)1);
}

void *
chanBlbTrnFdInputCtx(
  void *v
 ,int f
){
  return ((void *)(long)f);
  (void)v;
}

unsigned int
chanBlbTrnFdInput(
  void *v
 ,unsigned char *b
 ,unsigned int l
){
  int i;

  if ((i = read((int)(long)v, b, l)) < 0)
    i = 0;
  return (i);
}

void
chanBlbTrnFdInputClose(
  void *v
){
  close((int)(long)v);
}

void *
chanBlbTrnFdOutputCtx(
  void *v
 ,int f
){
  return ((void *)(long)f);
  (void)v;
}

unsigned int
chanBlbTrnFdOutput(
  void *v
 ,const unsigned char *b
 ,unsigned int l
){
  int i;

  if ((i = write((int)(long)v, b, l)) < 0)
    i = 0;
  return (i);
}

void
chanBlbTrnFdOutputClose(
  void *v
){
  close((int)(long)v);
}
