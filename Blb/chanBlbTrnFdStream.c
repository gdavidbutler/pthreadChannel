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
#include <sys/socket.h>
#include "chanBlbTrnFdStream.h"

void *
chanBlbTrnFdStreamCtx(
  int f
){
  return ((void *)(long)f);
}

void *
chanBlbTrnFdStreamInputCtx(
  void *v
){
  return (v);
}

unsigned int
chanBlbTrnFdStreamInput(
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
chanBlbTrnFdStreamInputClose(
  void *v
){
  shutdown((int)(long)v, SHUT_RD);
}

void *
chanBlbTrnFdStreamOutputCtx(
  void *v
){
  return (v);
}

unsigned int
chanBlbTrnFdStreamOutput(
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
chanBlbTrnFdStreamOutputClose(
  void *v
){
  shutdown((int)(long)v, SHUT_WR);
}

void
chanBlbTrnFdStreamFinalClose(
  void *v
){
  close((int)(long)v);
}
