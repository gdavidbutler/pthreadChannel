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
  void *frmCtx;
  chan_t *chan;
  void *outCtx;
  unsigned int (*out)(void *ctx, const void *buffer, unsigned int length);
  void (*fin)(void *chanBlbEgrCtx);
  void *opaque[4];
};

struct chanBlbIgrCtx {
  void *(*realloc)(void *, unsigned long);
  void (*free)(void *);
  void *frmCtx;
  chan_t *chan;
  void *inpCtx;
  unsigned int (*inp)(void *ctx, void *buffer, unsigned int length);
  chanBlb_t *blb;
  void (*fin)(void *chanBlbIgrCtx);
  void *opaque[4];
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
 *  Otherwise, output() is required, outputClose() is optional.
 * Provide an optional egress framer context
 * Provide an optional egress framer
 *
 * Provide an optional ingress chan_t: (if not provided, in parameters are not used)
 *  Otherwise, input() is required, inputClose() is optional.
 * Provide an optional ingress framer context
 * Provide an optional ingress framer
 * Provide an optional initial blob; previous input bytes from protocol start
 *
 * Provide an optional finalCtx and finalClose()
 *
 * A chanOpPut of chanBlb_t items on the egress channel does output():
 *  A chanOpGet or output() failure will chanShut(egress) and outputClose(outputCtx).
 * A chanOpGet on the ingress channel will return chanBlb_t items from input():
 *  A chanOpPut or input() failure will chanShut(ingress) and inputClose(inputCtx).
 * After all chanShut(), if provided, finlClose(finlCtx) is invoked
 *
 * Provide an optional pthread_create attribute
 */
int
chanBlb(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)

 ,chan_t *egress
 ,void *outputCtx
 ,unsigned int (*output)(void *outputCtx, const void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*outputClose)(void *outputCtx)
 ,void *egressFrmCtx
 ,void *(*egessrFrm)(struct chanBlbEgrCtx *)

 ,chan_t *ingress
 ,void *inputCtx
 ,unsigned int (*input)(void *inputCtx, void *buffer, unsigned int size) /* return 0 on failure */
 ,void (*inputClose)(void *inputCtx)
 ,void *igessrFrmCtx
 ,void *(*igessrFrm)(struct chanBlbIgrCtx *)
 ,chanBlb_t *blb

 ,void *finalCtx
 ,void (*finalClose)(void *finalCtx)

 ,pthread_attr_t *attr
); /* returns 0 on failure */

#endif /* __CHANBLB_H__ */
