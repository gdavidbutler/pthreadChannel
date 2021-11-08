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
  chanBlbFrmNf /* no framing, argument is readSize */
 ,chanBlbFrmNs /* NetString read and write framing, non-zero argument is read Blob maxSize, otherwise no arbitrary max */
 ,chanBlbFrmN0 /* NETCONF/1.0 read and write framing, non-zero argument is read Blob maxSize, otherwise no arbitrary max */
 ,chanBlbFrmN1 /* NETCONF/1.1 read and write framing, non-zero argument is read Blob maxSize, otherwise no arbitrary max */
 ,chanBlbFrmH1 /* HTTP/1.1 read framing and no write framing, non-zero argument is read Blob maxSize, otherwise no arbitrary max */
} chanBlbFrm_t;

/* Channel Blob
 *
 * Support input/output via ingress and egress channels.
 *
 * Provide an optional ingress chan_t: (if not provided, in parameters are not used)
 *  Otherwise, input() is required, inClose() is optional.
 * Provide an optional egress chan_t: (if not provided, out parameters are not used)
 *  Otherwise, output() is required, outShut() is optional.
 *
 * A chanOpPut of chanBlb_t items on the egress channel does output():
 *  A chanOpGet or output() failure will chanShut(egress) and outClose(out).
 *
 * A chanOpGet on the ingress channel will return chanBlb_t items from input():
 *  A chanOpPut or input() failure will chanShut(ingress) and inClose(in).
 *
 * After all chanShut(), if provided, finClose(fin) is invoked
 *
 * Provide an optional initialIngress; previous read bytes from protocol start
 */
int
chanBlb(
  chan_t *ingress
 ,void *in
 ,unsigned int (*input)(void *in, void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*inClose)(void *in)
 ,chan_t *egress
 ,void *out
 ,unsigned int (*output)(void *out, const void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*outClose)(void *out)
 ,void *fin
 ,void (*finClose)(void *out)
 ,chanBlbFrm_t framing
 ,unsigned int argument
 ,chanBlb_t *initialIngress
); /* returns 0 on failure */

#endif /* __CHANBLB_H__ */
