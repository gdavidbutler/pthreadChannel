/*
 * pthreadChannel - an implementation of CSP channels for pthreads
 * Copyright (C) 2019 G. David Butler <gdb@dbSystems.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __CHANSOCK_H__
#define __CHANSOCK_H__

/*
 * Channel Sock
 *
 * Support I/O on a bound full duplex socket via read and write channels.
 *
 * A chanPut() of chanSer_t items on the write channel does write()s on the socket:
 *  A chanGet() or socket write() failure will shutdown(socket, SHUT_WR) the socket and chanShut() the channel.
 *
 * A chanGet() on the read channel will return chanSer_t items from read()s on the socket:
 *  A chanPut() or socket read() failure will shutdown(socket, SHUT_RD) the socket and chanShut() the channel.
 *
 * After both ends have completed, the socket has been shutdown() but NOT closed.
 *
 * Provide:
 *  realloc semantics implementation function (or 0 to use system realloc)
 *  free semantics implementation function (or 0 to use system free)
 * Provide an optional read chan_t: (if not provided, socket is immediately shutdown(SHUT_RD))
 *   chanGet data that is read() from socket
 *   chanShut to shutdown(SHUT_RD) on socket
 * Provide an optional write chan_t: (if not provided, socket is immediately shutdown(SHUT_WR))
 *   chanPut data that is write() to socket
 *   chanShut to shutdown(SHUT_WR) on socket
 * Provide a socket to be used by the read and write channels:
 *  When read() on the socket fails, the socket is shutdown(SHUT_RD) and the read chan is chanShut()
 *  When write() on the socket fails, the socket is shutdown(SHUT_WR) and the write chan is chanShut()
 * Provide an optional non-zero readSize for reads from the socket (required if there is a read chan_t)
 *  If socket is a DGRAM type, this size must be at least as large as the largest expected payload size
 *
 * As a convenience, chanSock() chanOpen's the chan_t's (delegating chanClose's)
 */
int
chanSock(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chan_t *read
 ,chan_t *write
 ,int socket
 ,unsigned int readSize
); /* returns 0 on failure */

#endif /* __CHANSOCK_H__ */
