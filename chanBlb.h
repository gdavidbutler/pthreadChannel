/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2020 G. David Butler <gdb@dbSystems.com>
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
  chanBlbFrmNf /* no framing, argument is readSize (supports non-STREAM sockets) */
 ,chanBlbFrmNs /* NetString read and write framing, argument is read Blob maxSize, otherwise no arbitrary max */
 ,chanBlbFrmH1 /* HTTP/1.1 read framing, argument is read Blob maxSize, otherwise no arbitrary max */
} chanBlbFrm_t;

/*
 * Channel Sock
 *
 * Support I/O on a connected full duplex socket via ingress and egress channels.
 *
 * A chanOpPut of chanBlb_t items on the egress channel does write()s on the socketFd:
 *  A chanOpGet or write() failure will shutdown(socketFd, SHUT_WR) and chanShut() the egress channel.
 *
 * A chanOpGet on the ingress channel will return chanBlb_t items from read()s on the socketFd:
 *  A chanOpPut or read() failure will shutdown(socketFd, SHUT_RD) and chanShut() the ingress channel.
 *
 * After completion, the socketFd has been shutdown(), as above, but NOT closed.
 *
 * Provide:
 *  realloc semantics implementation function
 *  free semantics implementation function
 * Provide an optional ingress chan_t: (if not provided, socketFd will not be shutdown(SHUT_RD))
 *   chanOpGet data that is read() from socketFd
 *   chanShut() to shutdown(SHUT_RD) on socketFd
 * Provide an optional egress chan_t: (if not provided, socketFd will not be shutdown(SHUT_WR))
 *   chanOpPut data that is write() to socketFd
 *   chanShut() to shutdown(SHUT_WR) on socketFd
 * Provide a socketFd:
 *  When read() on the socketFd fails, the socketFd is shutdown(SHUT_RD) and the ingress chan is chanShut()
 *  When write() on the socketFd fails, the socketFd is shutdown(SHUT_WR) and the egress chan is chanShut()
 *
 * As a convenience, chanSock() chanOpen's the chan_t's (delegating chanClose's)
 */
int
chanSock(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chan_t *ingress
 ,chan_t *egress
 ,int socketFd
 ,chanBlbFrm_t framing
 ,int argument
); /* returns 0 on failure */

/*
 * Channel Pipe
 *
 * Support I/O on a pair of half duplex pipes via ingress and egress channels.
 *
 * A chanOpPut of chanBlb_t items on the egress channel does write()s on the writeFd:
 *  A chanOpGet or write() failure will close() the writeFd and chanShut() the egress channel.
 *
 * A chanOpGet on the ingress channel will return chanBlb_t items from read()s on the readFd:
 *  A chanOpPut or read() failure will close() the readFd and chanShut() the ingress channel.
 *
 * Provide:
 *  realloc semantics implementation function
 *  free semantics implementation function
 * Provide an optional ingress chan_t
 *   chanOpGet data that is read() from readFd
 *   chanShut() to close() readFd
 * Provide an optional egress chan_t
 *   chanOpPut data that is write() to writeFd
 *   chanShut() to close() writeFd
 * Provide readFd and writeFd:
 *  When read() on the readFd fails, the readFd is close() and the ingress chan is chanShut()
 *  When write() on the writeFd fails, the writeFd is close() and the egress chan is chanShut()
 *
 * As a convenience, chanPipe() chanOpen's the chan_t's (delegating chanClose's)
 */
int
chanPipe(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chan_t *ingress
 ,chan_t *egress
 ,int readFd
 ,int writeFd
 ,chanBlbFrm_t framing
 ,int argument
); /* returns 0 on failure */

#endif /* __CHANBLB_H__ */
