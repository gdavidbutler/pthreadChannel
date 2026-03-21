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

#ifndef __CHANBLBTRNFDDATAGRAM_H__
#define __CHANBLBTRNFDDATAGRAM_H__

void *
chanBlbTrnFdDatagramCtx(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
);

/*
 * Input: poll on all fds, recvfrom whichever is ready.
 * fd4/fd6: arrays of IPv4/IPv6 socket fds (copied internally).
 * fd4N/fd6N: counts (0 and 0 if none).
 */
void *
chanBlbTrnFdDatagramInputCtx(
  void *ctx
 ,const int *fd4
 ,const int *fd6
 ,unsigned int fd4N
 ,unsigned int fd6N
);

/* prepends source address to blob: [1 unsigned byte addrlen][addr] */
unsigned int
chanBlbTrnFdDatagramInput(
  void *inputCtx
 ,unsigned char *buffer
 ,unsigned int length
);

void
chanBlbTrnFdDatagramInputClose(
  void *inputCtx
);

/*
 * Output: send to all fds of matching address family.
 * fd4/fd6: arrays of IPv4/IPv6 socket fds (copied internally).
 * fd4N/fd6N: counts (0 and 0 if none).
 */
void *
chanBlbTrnFdDatagramOutputCtx(
  void *ctx
 ,const int *fd4
 ,const int *fd6
 ,unsigned int fd4N
 ,unsigned int fd6N
);

/* strips destination address from blob: [1 unsigned byte addrlen][addr] */
unsigned int
chanBlbTrnFdDatagramOutput(
  void *outputCtx
 ,const unsigned char *buffer
 ,unsigned int length
);

void
chanBlbTrnFdDatagramOutputClose(
  void *outputCtx
);

void
chanBlbTrnFdDatagramFinalClose(
  void *ctx
);

#endif /* __CHANBLBTRNFDDATAGRAM_H__ */
