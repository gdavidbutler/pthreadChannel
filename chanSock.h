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
 * A chanSend() of chanSockM_t messages on the write channel does write()s on the socket.
 * A charRecv() or socket write() failure will shutdown(fd, SHUT_WR) the socket and chanShut() the channel.
 *
 * A chanRecv() on the read channel will receive chanSockM_t messages from read()s on the socket.
 * A chanSend() or socket read() failure will shutdown(fd, SHUT_RD) the socket and chanShut() the channel.
 *
 * After both ends have completed, the socket is closed.
 */

/* a message, real size is sizeof(chanSockM_t) + (l ? l - 1 : 1) * sizeof(b[0]) */
/* a read limit is specified in the chanSock() call */
typedef struct {
  unsigned int l;
  unsigned char b[1]; /* the first character of l characters, must be last */
} chanSockM_t;

/* hand over a fd to be used by a pair of channels, when the channels are chanShut(), close() is called on the fd */
/* returns the pthread_t for the thread coordinating the channel threads */
pthread_t chanSock(void *(*realloc)(void *, unsigned long), void (*free)(void *), unsigned int readLimit, int fd, chan_t *read, chan_t *write); /* returns 0 on failure */

#endif /* __CHANSOCK_H__ */
