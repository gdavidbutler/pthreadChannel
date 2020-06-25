/*
 * pthreadChannel - Implementation of CSP channels for pthreads
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

/* channel store status */
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
 *  the realloc semantics implementation function (or 0 to use system realloc)
 *  the free semantics implementation function (or 0 to use system free)
 *  the store implementation function (or 0 if none)
 *  the store context (or 0 if none)
 *  the store context done function (or 0 if none)
 * For example:
 *  chan_t *c;
 *  c = chanCreate(0,0, chanFifoSi, chanFifoSa(10), chanFifoSd);
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
); /* returns 0 on failure */

/* channel open, to keep a channel from being deallocated till chanClose */
/* return chn as a convience to allow: chanDup = chanOpen(chan) */
chan_t *
chanOpen(
  chan_t *chn
);

/* channel shutdown, afterwards chanPut() always returns 0 and chanGet() is always non-blocking */
void
chanShut(
  chan_t *chn
);

/* channel close, on last close, deallocate */
void
chanClose(
  chan_t *chn
);

/* channel operation status */
typedef enum chanOs {
  chanOsErr = 0 /* Error */
 ,chanOsSht     /* Shutdown */
 ,chanOsGet     /* Get successful */
 ,chanOsPut     /* Put successful (or timeout on Wait part of PutWait) */
 ,chanOsPutWait /* PutWait successful */
 ,chanOsTmo     /* Timeout */
} chanOs_t;

/*
 * Channel poll
 */

/* channel poll operation */
typedef enum chanPo {
  chanPoNop = 0 /* no operation, skip */
 ,chanPoSht     /* check for shut */
 ,chanPoGet     /* get an item */
 ,chanPoPut     /* put an item */
 ,chanPoPutWait /* put an item then block till a Get occurs */
} chanPo_t;

/* channel poll array element */
typedef struct chanPoll {
  chan_t *c;  /* channel to operate on, if 0 then behave as chanPoNop */
  void **v;   /* where to get/put an item, if 0 then behave as chanPoNop */
  chanPo_t o;
  chanOs_t s;
} chanPoll_t;

/* in each of the below, nsTimeout: -1 block forever, 0 non-blocking else timeout in nanoseconds */

/*
 * Provide a set of channel operations and return when one of them completes.
 * Instead of having to change the size of the array, if no operation is desired
 * on a channel, set the chanPo to chanPoNop. Otherwise:
 *  When the store is full, Put blocks based on nsTimeout.
 *  When the store is empty, Get blocks based on nsTimeout.
 *  a PutWait is the same as a Put, then blocks till a Get occurs
 * Returns 0 on error (should only occur for memory allocation failures)
 *  otherwise the offset into the list is one less than the return value.
 */
unsigned int
chanPoll(
  long nsTimeout
 ,unsigned int count
 ,chanPoll_t *chnp
); /* returns 0 on failure */

/* check for shut */
chanOs_t
chanSht(
  long nsTimeout
 ,chan_t *chn
);

/* get an item */
chanOs_t
chanGet(
  long nsTimeout
 ,chan_t *chn
 ,void **val
);

/* put an item */
chanOs_t
chanPut(
  long nsTimeout
 ,chan_t *chn
 ,void *val
);

/* put an item (as chanPut) then block till a Get occurs */
chanOs_t
chanPutWait(
  long nsTimeout
 ,chan_t *chn
 ,void *val
);

#endif /* __CHAN_H__ */
