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

#ifndef __CHAN_H__
#define __CHAN_H__

/*
 * Channel Store
 */

/* channel store operation */
typedef enum chanSo {
  chanSoGet
 ,chanSoPut
} chanSo_t;

/* channel store wait */
typedef enum chanSw {
  chanSwNoGet = 1 /* no waiting Gets */
 ,chanSwNoPut = 2 /* no waiting Puts */
} chanSw_t;

/* channel store state */
typedef enum chanSs { /* bit map */
  chanSsCanPut = 1 /* has room */
 ,chanSsCanGet = 2 /* has content */
} chanSs_t;

/* channel store implementation
 *
 * A channel store takes a pointer to a context,
 *  the operation the channel wants to perform on the store
 *  indication of waiting Gets and Puts
 *  and a value pointer
 * It returns the state of the store as it relates to Get and Put.
 * The store is called under protection of a channel operation mutex.
 */
typedef chanSs_t
(*chanSi_t)(
  void *cntx
 ,chanSo_t oper
 ,chanSw_t wait
 ,void **val
);

/* channel store context done, called during last (deallocating) chanClose */
typedef void
(*chanSd_t)(
  void *cntx
);

/*
 * Channel
 */
typedef struct chan chan_t;

/*
 * A store can be provided at channel allocation.
 *  If none is provided, a channel stores a single item.
 *  This works best (providing low latency) when threads work more and talk less.
 *
 * When allocating the channel, supply:
 *  the realloc semantics implementation function
 *  the free semantics implementation function
 *  the store implementation function (or 0 if none)
 *  the store context (or 0 if none)
 *  the store context done function (or 0 if none)
 *
 * Returned channel is Open.
 */
chan_t *
chanCreate(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,chanSi_t impl
 ,void *cntx
 ,chanSd_t done
);

/* channel (re)Open, to keep a channel from being deallocated till chanClose */
/* return chn as a convience to allow: chanDup = chanOpen(chan) */
chan_t *
chanOpen(
  chan_t *chn
);

/* channel shutdown, afterwards chanOpPut returns chanOsSht and chanOpGet is non-blocking */
void
chanShut(
  chan_t *chn
);

/* channel close, on last close, deallocate */
void
chanClose(
  chan_t *chn
);

/* channel operation */
typedef enum chanOp {
  chanOpNop = 0 /* no operation, skip */
 ,chanOpSht     /* Shutdown [event only, use chanShut() to shutdown a channel] */
 ,chanOpGet     /* Get (event: store empty, queued Gets and no queued Puts) */
 ,chanOpPut     /* Put (event: store full, queued Puts and no queued Gets) */
} chanOp_t;

/* channel operation state */
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
 * val is where to get/put an item or 0 for event
 */
chanOs_t
chanOp(
  long nsTimeout
 ,chan_t *chan
 ,void **val
 ,chanOp_t op
);

/* channel array */
typedef struct chanArr {
  chan_t *c;  /* channel to operate on, 0 == chanOpNop */
  void **v;   /* where to get/put or 0 for event */
  chanOp_t o;
  chanOs_t s;
} chanArr_t;

/*
 * Operate on one (first capable) Channal of a Channal array based on nsTimeout:
 *  >0 timeout in nanoseconds
 *   0 block
 *  -1 non-blocking
 * Returns 0 on error (memory allocation failure).
 * Otherwise the offset into the list is one less than the return value.
 */
unsigned int
chanOne(
  long nsTimeout
 ,unsigned int count
 ,chanArr_t *array
);

/* chanAll return */
typedef enum chanMs {
  chanMsErr = 0 /* memory allocation failure or deadlock */
 ,chanMsEvt     /* Event - check chanOs_t */
 ,chanMsOp      /* Operation - check chanOs_t */
 ,chanMsTmo     /* timeout */
} chanMs_t;

/*
 * Operate on all Channals of a Channal array based on nsTimeout:
 *  >0 timeout in nanoseconds
 *   0 block
 *  -1 non-blocking
 */
chanMs_t
chanAll(
  long nsTimeout
 ,unsigned int count
 ,chanArr_t *array
);

#endif /* __CHAN_H__ */
