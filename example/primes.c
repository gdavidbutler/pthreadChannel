/*
 * pthreadChannel - an implementation of CSP/agent channels for pthreads
 * Copyright (C) 2018 G. David Butler <gdb@dbSystems.com>
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

/* Based on https://swtch.com/libtask/primes.c */

#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "chan.h"

/*
 * Example that uses a chain of threads connected with channels
 * (like a bucket brigade) to print prime numbers up to a
 * command line specified goal.
 * (This is a really bad way of finding primes, by the way.)
 *
 * Each thread gets a number that the previous thread in the chain
 * couldn't evenly divide with it's number.
 * And, by definition, that number is prime. After that, it simply
 * drops those that are evenly divided by its number and
 * passes the rest down the chain.
 *
 * The main function simply sends odd numbers into the head of the chain.
 * (a small optimization since 2 is the only even prime)
 */

/*
 * Since this example talks more and works less (see the opposite comment in chan.h),
 * using a FIFO queue drastically decreases thread context switching.
 * Since each prime thread is a filter, there are many more messages at the head of
 * the chain than at the end. Queues are sized relative to the length of the chain.
 */
#define QUE 1 /* 0 or 1 to use a queue */
#if QUE
#include "chanFifo.h"
#endif

unsigned int Goal; /* use how far to go to size the queues */

#define POLL 1 /* 0 or 1 to use chanPoll */

/*
 * The channel design is based on pointer messages. The expected case is to malloc a message
 * and pass it into the channel. Therefore, the semantics include delegation of ownership
 * to the receiver. Yes, the receiver does the free().
 *
 * Of course, if the meesage fits in a void*, then with proper casting, a value can be sent.
 * Not advised, but it can be done.
 */
#define MEMORY 1 /* 0 or 1 to delegate malloc'd memory or cast unsigned long to void* */

void *
primeT(
  void *v
){
#if MEMORY
  unsigned int *ip;
#else
  unsigned long ip;
#endif
  pthread_t t;
  unsigned int prime;
  int r;
#if POLL
  chanPoll_t c[2]; /* 0 is the read channel and 1 is the write channel */

  c[0].c = v;
  c[0].v = (void **)&ip;
  c[0].o = chanOpRecv;
  c[1].c = 0;
  c[1].v = (void **)&ip;
  c[1].o = chanOpNoop;
  if (!chanPoll(0, sizeof(c) / sizeof(c[0]), c))
    return 0;
#if QUE
#if MEMORY
  if ((prime = (Goal - *ip) / 10) > 1)
#else
  if ((prime = (Goal - ip) / 10) > 1)
#endif
    c[1].c = chanAlloc(realloc, free, chanFifoQi, chanFifoQa(realloc, free, prime), chanFifoQd);
  else
#endif
    c[1].c = chanAlloc(realloc, free, 0, 0, 0);
  assert(c[1].c);
  pthread_cleanup_push((void(*)(void*))chanFree, c[1].c);
#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  if (pthread_create(&t, 0, primeT, c[1].c)) {
    puts("Can't create more threads, draining pipeline...");
    while (chanPoll(0, sizeof(c) / sizeof(c[0]), c))
#if MEMORY
      free(ip)
#endif
      ;
    goto exit;
  }
  for (;;) {
    switch (chanPoll(0, sizeof(c) / sizeof(c[0]), c)) {
    case 1:
#if MEMORY
      if (*ip % prime)
#else
      if (ip % prime)
#endif
      {
        c[0].o = chanOpNoop;
        c[1].o = chanOpSend;
      }
#if MEMORY
      else
        free(ip);
#endif
      break;
    case 2:
      c[0].o = chanOpRecv;
      c[1].o = chanOpNoop;
      break;
    default:
      goto endFor;
      break;
    }
  }
endFor:
  chanShut(c[1].c);
  r = pthread_join(t, 0);
  assert(!r);
  printf("joined %d\n", prime); fflush(stdout);
exit:
  pthread_cleanup_pop(1); /* chanFree(c[1].c) */

#else /* POLL */
  chan_t *c;

  if (!chanRecv(0, v, (void **)&ip))
    return 0;
#if QUE
#if MEMORY
  if ((prime = (Goal - *ip) / 10) > 1)
#else
  if ((prime = (Goal - ip) / 10) > 1)
#endif
    c = chanAlloc(realloc, free, chanFifoQi, chanFifoQa(realloc, free, prime), chanFifoQd);
  else
#endif
    c = chanAlloc(realloc, free, 0, 0, 0);
  assert(c);
  pthread_cleanup_push((void(*)(void*))chanFree, c);
#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  if (pthread_create(&t, 0, primeT, c)) {
    puts("Can't create more threads, draining pipeline...");
    while (chanRecv(0, v, (void **)&ip))
#if MEMORY
      free(ip)
#endif
      ;
    goto exit;
  }
  for (;;) {
    if (!chanRecv(0, v, (void **)&ip))
      break;
#if MEMORY
    if (*ip % prime)
#else
    if (ip % prime)
#endif
    {
#if MEMORY
      r = chanSend(0, c, ip);
#else
      r = chanSend(0, c, (void *)ip);
#endif
      assert(r);
    }
#if MEMORY
      else
        free(ip);
#endif
  }
  chanShut(c);
  r = pthread_join(t, 0);
  assert(!r);
  printf("joined %d\n", prime); fflush(stdout);
exit:
  pthread_cleanup_pop(1); /* chanFree(c) */

#endif /* POLL */
  return 0;
}

int
main(
  int argc
 ,char **argv
){
  chan_t *c;
  pthread_t t;
  unsigned int i;
  int r;

  if (argc < 2
   || (Goal = atoi(*(argv + 1))) < 2)
    Goal = 100;
  printf("Goal = %d\n", Goal);
#if QUE
  if ((i = Goal / 10) > 1)
    c = chanAlloc(realloc, free, chanFifoQi, chanFifoQa(realloc, free, i), chanFifoQd);
  else
#endif
    c = chanAlloc(realloc, free, 0, 0, 0);
  assert(c);
  pthread_cleanup_push((void(*)(void*))chanFree, c);
  r = pthread_create(&t, 0, primeT, c);
  assert(!r);
  puts("2");
  for (i = 3; i <= Goal; i += 2) {
#if MEMORY
    unsigned int *ip;
#else
    unsigned long ip;
#endif

#if MEMORY
    ip = malloc(sizeof(*ip));
    assert(ip);
    *ip = i;
    r = chanSend(0, c, ip);
#else
    ip = i;
    r = chanSend(0, c, (void *)ip);
#endif
    assert(r);
  }
  chanShut(c);
  r = pthread_join(t, 0);
  assert(!r);
  printf("joined Goal\n"); fflush(stdout);
  pthread_cleanup_pop(1); /* chanFree(c.c) */
  return 0;
}
