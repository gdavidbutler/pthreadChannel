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

#ifndef __CHANBLBTRNFD_H__
#define __CHANBLBTRNFD_H__

void *
chanBlbTrnFdCtx(
  void
);

void *
chanBlbTrnFdInputCtx(
  void *context
 ,int inputFd
);

unsigned int
chanBlbTrnFdInput(
 void *inputCtx
,unsigned char *buffer
,unsigned int length
);

void
chanBlbTrnFdInputClose(
  void *inputCtx
);

void *
chanBlbTrnFdOutputCtx(
  void *context
 ,int outputFd
);

unsigned int
chanBlbTrnFdOutput(
  void *outputCtx
 ,const unsigned char *buffer
 ,unsigned int length
);

void
chanBlbTrnFdOutputClose(
  void *outputCtx
);

#define chanBlbTrnFdFinalClose 0

#endif /* __CHANBLBTRNFD_H__ */
