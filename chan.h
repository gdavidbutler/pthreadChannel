/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2024 G. David Butler <gdb@dbSystems.com>
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

#ifndef __CHAN_H__
#define __CHAN_H__

/*
 * Channel Store
 */

/* Channel Store state */
typedef enum chanSs { /* bit map */
  chanSsCanPut = 1 /* not full */
 ,chanSsCanGet = 2 /* not empty */
} chanSs_t;

/* Channel Store deallocation
 * called during last (deallocating) chanClose
 *
 * takes:
 *  a pointer to a Store closure,
 */
typedef void
(*chanSd_t)(
  void *storeClosure
 ,chanSs_t state
);

/* Channel Store operation */
typedef enum chanSo {
  chanSoGet
 ,chanSoPut
} chanSo_t;

/* Channel Store wait */
typedef enum chanSw { /* bit map */
  chanSwNoGet = 1 /* no waiting Gets */
 ,chanSwNoPut = 2 /* no waiting Puts */
} chanSw_t;

/* Channel Store implementation
 * called to perform Store operation and release begin stablity
 *
 * takes:
 *  a pointer to a Store closure,
 *  the operation the Channel wants to perform on the Store
 *  indication of waiting Gets and Puts
 *  and a value pointer
 * Return the state of the Store as it relates to Get and Put.
 *  if zero, shutdown the channel
 */
typedef chanSs_t
(*chanSi_t)(
  void *storeClosure
 ,chanSo_t oper
 ,chanSw_t wait
 ,void **val
);

/* Channel Store allocation
 * called to allocate a Channel Store
 *
 * receives:
 *  a realloc() like function (from chanCreate)
 *  a free() like function (from chanCreate)
 *  a Store item dequeue function (from chanCreate)
 *  a wake function
 *  a wake closure
 *
 * provides:
 *  a Store deallocation function or zero
 *  a Store implementation function
 *  a Store closure
 *
 * takes additioal specific arguments
 *
 * Return a Store state
 *  if zero, failed
 */
typedef chanSs_t
(*chanSa_t)(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,void (*dequeue)(void *)
 ,int (*wake)(void *, chanSs_t)
 ,void *wakeClosure
 ,chanSd_t *deallocation
 ,chanSi_t *implementation
 ,void **storeClosure
 ,va_list
);

/*
 * Channel Init
 *
 * Must be called to provide thread safe realloc and free implementations
 */
void
chanInit(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
);

/*
 * Channel
 */
typedef struct chan chan_t;

/*
 * A Store can be provided at Channel allocation.
 *  If none is provided, a Channel stores a single item.
 *  This works best (providing low latency) when threads work more and talk less.
 *
 * When allocating the Channel, supply:
 *  a Store item dequeue function (0 if none)
 *  a Store allocation function (0 if none)
 *  additional allocation parameters
 *
 * Return 0 on error (memory allocation)
 * Returned Channel is Open.
 */
chan_t *
chanCreate(
  void (*dequeue)(void *)
 ,chanSa_t allocation
 ,...
);

/* Channel (re)Open, to keep a Channel from being deallocated till chanClose */
/* Calling with 0 is a harmless no-op */
/* Return chn as a convience to allow: chanDup = chanOpen(chan) */
chan_t *
chanOpen(
  chan_t *chn
);

/* Channel shutdown */
/* then chanOpPut returns chanOsSht and chanOpGet is non-blocking */
/* Calling with 0 is a harmless no-op */
void
chanShut(
  chan_t *chn
);

/* Channel close, on last close, deallocate */
/* calling with 0 is a harmless no-op */
void
chanClose(
  chan_t *chn
);

/* Channel number of chanOpen not yet chanClose (chanClose at zero deallocates) */
unsigned int
chanOpenCnt(
  chan_t *chn
);

/* Channel operation */
typedef enum chanOp {
  chanOpNop = 0 /* no operation, skip */
 ,chanOpSht     /* monitor for Shutdown */
 ,chanOpGet     /* Get or Get demand (queued Puts on full store) */
 ,chanOpPut     /* Put or Put demand (queued Gets on empty store) */
} chanOp_t;

/* Channel operation status */
typedef enum chanOs {
  chanOsNop = 0 /* None of the below */
 ,chanOsSht     /* Shutdown */
 ,chanOsGet     /* Get */
 ,chanOsPut     /* Put */
 ,chanOsTmo     /* Timeout */
} chanOs_t;

/*
 * Operate on a Channel based on nsTimeout:
 *  >0 timeout in nanoseconds
 *   0 block
 *  -1 non-blocking
 * val is where to get/put an item or 0 for monitor
 */
chanOs_t
chanOp(
  long nsTimeout
 ,chan_t *chan
 ,void **val
 ,chanOp_t op
);

/* Channel array */
typedef struct chanArr {
  chan_t *c;  /* channel to operate on, 0 == chanOpNop */
  void **v;   /* where to get/put or 0 for monitor */
  void *x;    /* application closure - not used by channels */
  chanOp_t o;
  chanOs_t s;
} chanArr_t;

/*
 * Operate on one (first capable) Channal of a Channal array based on nsTimeout:
 *  >0 timeout in nanoseconds
 *   0 block
 *  -1 non-blocking
 * Return 0 on error (memory allocation failure).
 * Otherwise the offset into the list is one less than the return value.
 */
unsigned int
chanOne(
  long nsTimeout
 ,unsigned int count
 ,chanArr_t *array
);

/* chanAll status */
typedef enum chanAl {
  chanAlErr = 0 /* memory allocation failure */
 ,chanAlEvt     /* Event - check chanOs_t */
 ,chanAlOp      /* Operation - check chanOs_t */
 ,chanAlTmo     /* timeout */
} chanAl_t;

/*
 * Operate on all Channals of a Channal array based on nsTimeout:
 *  >0 timeout in nanoseconds
 *   0 block
 *  -1 non-blocking
 */
chanAl_t
chanAll(
  long nsTimeout
 ,unsigned int count
 ,chanArr_t *array
);

#endif /* __CHAN_H__ */
