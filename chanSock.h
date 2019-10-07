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
 * Support I/O on a bound full duplex socket via a pair of send and receive channels.
 *
 * A chanPut() of chanSockM_t messages on the write channel does write()s on the socket:
 *  A chanGet() or socket write() failure will shutdown(socket, SHUT_WR) the socket and chanShut() the channel.
 *
 * A chanGet() on the read channel will receive chanSockM_t messages from read()s on the socket:
 *  A chanPut() or socket read() failure will shutdown(socket, SHUT_RD) the socket and chanShut() the channel.
 *
 * After both ends have completed, the socket is closed.
 */

/* a message, real size is sizeof (chanSockM_t) + (l ? l - 1 : 1) * sizeof (b[0]) */
/* a read limit is specified in the chanSock() call */
typedef struct chanSockM {
  unsigned int l;
  unsigned char b[1]; /* the first character of l characters, must be last */
} chanSockM_t;

/*
 * Provide a hangup chan_t to the thread coordinating the I/O threads:
 *  chanShut() on the hangup channel does chanShut() on the std channels and the socket is shutdown(SHUT_RDWR) and close()'d
 *  chanGet() on the hangup channel to block for chanShut when the chanSock completes
 * Provide a socket to be used by a pair of channels:
 *  When read() on the socket fails, the socket is shutdown(SHUT_RD) and the read chan is chanShut()
 *  When write() on the socket fails, the socket is shutdown(SHUT_WR) and the write chan is chanShut()
 * Provide a read chan_t
 * Provide a write chan_t
 * Provide a readLimit to set/limit the size of a read from the socket
 *
 * chanSock() takes care of calling chanOpen on each chan_t for the sub-threads
 */
int
chanSock(
  chan_t *hangup
 ,int socket
 ,chan_t *read
 ,chan_t *write
 ,unsigned int readLimit
); /* returns 0 on failure */

#endif /* __CHANSOCK_H__ */
