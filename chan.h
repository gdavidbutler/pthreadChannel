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
 * Channel Queue.
 */

/* channel queue operation */
typedef enum chanQo {
  chanQoGet
 ,chanQoPut
} chanQo_t;

/* channel queue status */
typedef enum chanQs { /* bit map */
  chanQsCanPut = 1 /* has room */
 ,chanQsCanGet = 2 /* has content */
} chanQs_t;

/* channel queue implementation
 *
 * A channel queue takes a pointer to a context,
 * the operation the channel wants to perform on the queue
 * and a value pointer
 * It returns the state of the queue as it relates to Get and Put.
 * The queue is called under protection of a channel operation mutex
 */
typedef chanQs_t (*chanQi_t)(void *cntx, chanQo_t oper, void **val);

/* channel queue context done, called during channel done */
typedef void (*chanQd_t)(void *cntx);

/*
 * Channel.
 */
typedef struct chan chan_t;

/*
 * A queue can be provided at channel allocation.
 * If none is provided, a channel queues a single message.
 * This works best (providing low latency) when threads work more and talk less.
 *
 * When allocating the channel, supply:
 *  a pointer to a function with realloc() semantics
 *  a pointer to a function with free() semantics
 *  the queue implementation function (or 0 if none)
 *  the queue context (or 0 if none)
 *  the queue context free function (or 0 if none)
 * For example:
 *  chan_t *c;
 *  c = chanAlloc(realloc, free, chanFifoQi, chanFifoQa(realloc, free, 10), chanFifoQf);
 */
chan_t *chanAlloc(void *(*realloc)(void *, unsigned long), void (*free)(void *), chanQi_t impl, void *cntx, chanQd_t done); /* returns 0 on failure */

/* channel done */
int chanDone(chan_t *chn); /* returns 0 on busy */

/*
 * Channels distribute messages fairly under pressure.
 *  If there are waiting readers, a new reader goes to the end of the line
 *   unless there are also waiting writers (waiting readers won't wait long)
 *    then a meesage is opportunistically received instead of forcing a wait.
 *  If there are waiting writers, a new writers goes to the end of the line
 *   unless there are also waiting readers (waiting writers won't wait long)
 *    then a meesage is opportunistically sent instead of forcing a wait.
 */

/* receive a message */
int chanRecv(int noblock, chan_t *chn, void **val); /* returns 0 on failure */

/* send a message */
int chanSend(int noblock, chan_t *chn, void *val); /* returns 0 on failure */

/* send a message then block return till a Recv occurs (for synchronization) */
int chanSendWait(int noblock, chan_t *chn, void *val); /* returns 0 on failure */

/*
 * Channel poll.
 */

/* channel poll operation */
typedef enum chanOp {
  chanOpNoop     /* no operation, skip */
 ,chanOpRecv     /* receive a message */
 ,chanOpSend     /* send a message */
 ,chanOpSendWait /* send a message then block return till a Recv occurs (for synchronization) */
} chanOp_t;

/* channel poll array element */
typedef struct chanPoll {
  chan_t *c;
  void **v;
  chanOp_t o;
} chanPoll_t;

/*
 * Provide a set of channel operations and return when one of them completes.
 * Instead of having to change the size of the set, if no operation is desired
 * on a channel, set the Op to Noop. Otherwise:
 *  When the queue is full, Send blocks unless noblock is set.
 *  When the queue is empty, Recv blocks unless noblock is set.
 *  SendWait does a Send then blocks return till a Recv occurs (for synchronization)
 * If an operation is successful (return greater than 0),
 * the offset into the list is one less than the return value.
 */
int chanPoll(int noblock, unsigned int count, chanPoll_t *chnp); /* returns 0 on failure */

#endif /* __CHAN_H__ */
