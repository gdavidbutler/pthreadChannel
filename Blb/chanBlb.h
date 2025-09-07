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

/**********************************************************/

struct chanBlbEgrCtx {
  void *(*realloc)(void *, unsigned long);
  void (*free)(void *);
  chan_t *chan;
  void *ctx;
  unsigned int (*output)(void *ctx, const void *buffer, unsigned int length);
  void *opaque[4];
  void (*fin)(void *chanBlbEgrCtx);
};

struct chanBlbIgrCtx {
  void *(*realloc)(void *, unsigned long);
  void (*free)(void *);
  chan_t *chan;
  void *ctx;
  unsigned int (*input)(void *ctx, void *buffer, unsigned int length);
  void *opaque[4];
  void (*fin)(void *chanBlbIgrCtx);
  chanBlb_t *blb; /* initial ingress */
  unsigned int arg;
};

/* utility to injest chanBlbIgrCtx->blb */
unsigned int
chanBlbIgrBlb(
  void (*free)(void *)
 ,chanBlb_t **blob
 ,void *destination
 ,unsigned int len
);

/* Channel Blob
 *
 * Support input/output via ingress and egress channels.
 *
 * Provide realloc and free routines to use.
 *
 * Provide an optional egress chan_t: (if not provided, out parameters are not used)
 *  Otherwise, output() is required, outClose() is optional.
 * A chanOpGet on the ingress channel will return chanBlb_t items from input():
 *  A chanOpPut or input() failure will chanShut(ingress) and inClose(in).
 * Provide an optional egress framer
 *
 * Provide an optional ingress chan_t: (if not provided, in parameters are not used)
 *  Otherwise, input() is required, inClose() is optional.
 * A chanOpPut of chanBlb_t items on the egress channel does output():
 *  A chanOpGet or output() failure will chanShut(egress) and outClose(out).
 * Provide an optional ingress framer
 * Provide an optional initialIngress; previous input bytes from protocol start
 *
 * After all chanShut(), if provided, finClose(fin) is invoked
 *
 * Provide an optional pthread_create attribute
 * Provide an optional framer argument
 */
int
chanBlb(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chan_t *egress
 ,void *out
 ,unsigned int (*output)(void *out, const void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*outClose)(void *out)
 ,void *(*egressFrm)(struct chanBlbEgrCtx *)
 ,chan_t *ingress
 ,void *in
 ,unsigned int (*input)(void *in, void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*inClose)(void *in)
 ,void *(*ingressFrm)(struct chanBlbIgrCtx *)
 ,chanBlb_t *initialIngress
 ,void *fin
 ,void (*finClose)(void *out)
 ,pthread_attr_t *attr
 ,unsigned int argument
); /* returns 0 on failure */

#endif /* __CHANBLB_H__ */
