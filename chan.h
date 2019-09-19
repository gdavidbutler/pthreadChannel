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
 * Channel Store.
 */

/* channel store operation */
typedef enum chanSo {
  chanSoPull
 ,chanSoPush
} chanSo_t;

/* channel store status */
typedef enum chanSs { /* bit map */
  chanSsCanPush = 1 /* has room */
 ,chanSsCanPull = 2 /* has content */
} chanSs_t;

/* channel store implementation
 *
 * A channel store takes a pointer to a context,
 * the operation the channel wants to perform on the store
 * and a value pointer
 * It returns the state of the store as it relates to Pull and Push.
 * The store is called under protection of a channel operation mutex
 */
typedef chanSs_t (*chanSi_t)(void *cntx, chanSo_t oper, void **val);

/* channel store context done, called during channel done */
typedef void (*chanSd_t)(void *cntx);

/*
 * Channel.
 */
typedef struct chan chan_t;

/*
 * A store can be provided at channel allocation.
 * If none is provided, a channel stores a single message.
 * This works best (providing low latency) when threads work more and talk less.
 *
 * When allocating the channel, supply:
 *  a pointer to a function with realloc() semantics
 *  a pointer to a function with free() semantics
 *  the store implementation function (or 0 if none)
 *  the store context (or 0 if none)
 *  the store context free function (or 0 if none)
 * For example:
 *  chan_t *c;
 *  c = chanCreate(realloc, free, chanFifoSi, chanFifoSa(realloc, free, 10), chanFifoSf);
 *
 * returned channel is Open
 */
chan_t *chanCreate(void *(*realloc)(void *, unsigned long), void (*free)(void *), chanSi_t impl, void *cntx, chanSd_t done); /* returns 0 on failure */

/* channel open, to keep a channel from being deallocated till chanClose */
void chanOpen(chan_t *chn);

/* channel shutdown, afterwards chanPush() returns 0 and chanPull() is noblock */
void chanShut(chan_t *chn);

/* channel is shutdown, when a chan Push/Pull return 0, use this to see if this is the reason */
int chanIsShut(chan_t *chn);

/* channel close, on last close, deallocate */
void chanClose(chan_t *chn);

/*
 * Channels distribute messages fairly under pressure.
 *  If there are waiting readers, a new reader goes to the end of the line
 *   unless there are also waiting writers (waiting readers won't wait long)
 *    then a meesage is opportunistically pulled instead of forcing a wait.
 *  If there are waiting writers, a new writers goes to the end of the line
 *   unless there are also waiting readers (waiting writers won't wait long)
 *    then a meesage is opportunistically sent instead of forcing a wait.
 */

/* pull a message */
unsigned int chanPull(int noblock, chan_t *chn, void **val); /* returns 0 on failure, see chanIsShut() */

/* push a message */
unsigned int chanPush(int noblock, chan_t *chn, void *val); /* returns 0 on failure, see chanIsShut() */

/* push a message then block return till a Pull occurs (for synchronization) */
unsigned int chanPushWait(int noblock, chan_t *chn, void *val); /* returns 0 on failure, see chanIsShut() */

/*
 * Channel poll.
 */

/* channel poll operation */
typedef enum chanOp {
  chanOpNoOp     /* no operation, skip */
 ,chanOpPull     /* pull a message */
 ,chanOpPush     /* push a message */
 ,chanOpPushWait /* push a message then block return till a Pull occurs (for synchronization) */
} chanOp_t;

/* channel poll array element */
typedef struct chanPoll {
  chan_t *c;  /* channel to operate on, if 0 then chanOpNoOp */
  void **v;   /* where to get/put a message, if 0 then chanOpNoOp */
  chanOp_t o;
} chanPoll_t;

/*
 * Provide a set of channel operations and return when one of them completes.
 * Instead of having to change the size of the set, if no operation is desired
 * on a channel, set the Op to NoOp. Otherwise:
 *  When the store is full, Push blocks unless noblock is set.
 *  When the store is empty, Pull blocks unless noblock is set.
 *  PushWait does a Push then blocks return till a Pull occurs (for synchronization)
 * If an operation is successful (return greater than 0),
 * the offset into the list is one less than the return value.
 */
unsigned int chanPoll(int noblock, unsigned int count, chanPoll_t *chnp); /* returns 0 on failure, see chanIsShut() */

#endif /* __CHAN_H__ */
