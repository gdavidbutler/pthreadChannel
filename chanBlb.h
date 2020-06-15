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

#ifndef __CHANBLB_H__
#define __CHANBLB_H__

/* a blob (a length prefixed array of bytes) */
typedef struct {
  unsigned int l;     /* not transmitted, no byte order issues */
  unsigned char b[1]; /* the first character of l characters */
} chanBlb_t;

/* return size to allocate a chanBlb_t to contain a given number of octets (to overcome padding issues) */
unsigned int
chanBlb_tSize(
  unsigned int octets
);

typedef enum {
  chanBlbFrmNf /* no framing, argument is readSize */
 ,chanBlbFrmNs /* NetString read and write framing, argument is read maxSize */
 ,chanBlbFrmH1 /* Http1 read framing, argument is read maxSize, when zero header is limited to 16k and trailer is limited to 8k */
} chanBlbFrm_t;

/*
 * Channel Sock
 *
 * Support I/O on a connected full duplex socket via read and write channels.
 *
 * A chanPut() of chanBlb_t items on the write channel does write()s on the socket:
 *  A chanGet() or socket write() failure will shutdown(socket, SHUT_WR) the socket and chanShut() the channel.
 *
 * A chanGet() on the read channel will return chanBlb_t items from read()s on the socket:
 *  A chanPut() or socket read() failure will shutdown(socket, SHUT_RD) the socket and chanShut() the channel.
 *
 * After completion, the socket has been shutdown(), as above, but NOT closed.
 *
 * Provide:
 *  realloc semantics implementation function (or 0 to use system realloc)
 *  free semantics implementation function (or 0 to use system free)
 * Provide an optional read chan_t: (if not provided, socket will not be shutdown(SHUT_RD))
 *   chanGet data that is read() from socket
 *   chanShut to shutdown(SHUT_RD) on socket
 * Provide an optional write chan_t: (if not provided, socket will not be shutdown(SHUT_WR))
 *   chanPut data that is write() to socket
 *   chanShut to shutdown(SHUT_WR) on socket
 * Provide a socket to be used by the read and write channels:
 *  When read() on the socket fails, the socket is shutdown(SHUT_RD) and the read chan is chanShut()
 *  When write() on the socket fails, the socket is shutdown(SHUT_WR) and the write chan is chanShut()
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
 ,chanBlbFrm_t framing
 ,int argument
); /* returns 0 on failure */

/*
 * Channel Pipe
 *
 * Support I/O on a pair of half duplex pipes via read and write channels.
 *
 * A chanPut() of chanBlb_t items on the write channel does write()s on the writeFd:
 *  A chanGet() or writeFd write() failure will close() the writeFd and chanShut() the channel.
 *
 * A chanGet() on the read channel will return chanBlb_t items from read()s on the readFd:
 *  A chanPut() or readFd read() failure will close() the readFd and chanShut() the channel.
 *
 * Provide:
 *  realloc semantics implementation function (or 0 to use system realloc)
 *  free semantics implementation function (or 0 to use system free)
 * Provide an optional read chan_t
 *   chanGet data that is read() from readFd
 *   chanShut to close() readFd
 * Provide an optional write chan_t
 *   chanPut data that is write() to writeFd
 *   chanShut to close() writeFd
 * Provide readFd and writeFd to be used by the read and write channels:
 *  When read() on the readFd fails, the read chan is chanShut()
 *  When write() on the writeFd fails, the write chan is chanShut()
 *
 * As a convenience, chanPipe() chanOpen's the chan_t's (delegating chanClose's)
 */
int
chanPipe(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chan_t *read
 ,chan_t *write
 ,int readFd
 ,int writeFd
 ,chanBlbFrm_t framing
 ,int argument
); /* returns 0 on failure */

#endif /* __CHANBLB_H__ */
