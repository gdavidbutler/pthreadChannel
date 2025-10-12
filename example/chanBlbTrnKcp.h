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

#ifndef __CHANBLBTRNKCP_H__
#define __CHANBLBTRNKCP_H__

void *
chanBlbTrnKcpCtx(
  void *(*malloc)(unsigned long)
 ,void (*free)(void *)
 ,int fd /* datagram socket */
 ,unsigned int cid /* connection id */
 ,unsigned short inwnd /* 0 -> 128 */
 ,unsigned short outwnd /* 0 -> 32 */
 ,unsigned short mtu /* 0 -> 1400 */
 ,unsigned short nodelay /* norm:0 fast:1 */
 ,unsigned short interval /* norm:0 fast:10 */
 ,unsigned short resend /* norm:0 fast:2 */
 ,unsigned short nocongestion /* norm:0 fast:1 */
);

void *
chanBlbTrnKcpInputCtx(
  void *context
);

unsigned int
chanBlbTrnKcpInput(
  void *inputCtx
 ,unsigned char *buffer
 ,unsigned int length
);

void
chanBlbTrnKcpInputClose(
  void *inputCtx
);

void *
chanBlbTrnKcpOutputCtx(
  void *context
);

unsigned int
chanBlbTrnKcpOutput(
  void *outputCtx
 ,const unsigned char *buffer
 ,unsigned int length
);

void
chanBlbTrnKcpOutputClose(
  void *outputCtx
);

void
chanBlbTrnKcpFinalClose(
  void *context
);

#endif /* __CHANBLBTRNKCP_H__ */
