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
 * using a FIFO store drastically decreases thread context switching.
 * Since each prime thread is a filter, there are many more messages at the head of
 * the chain than at the end. Stores are sized relative to the length of the chain.
 * Since latency is not important (throughput is), the working size is set to max.
 */
#define STORE 1 /* 0 or 1 to use a store */
#if STORE
#include "chanFifo.h"
#endif

/*
 * The channel design is based on pointer messages. The expected case is to malloc a message
 * and pass it into the channel. Therefore, the semantics include delegation of ownership
 * to the receiver. Yes, the receiver does the free().
 *
 * Of course, if the meesage fits in a void*, then with proper casting, a value can be sent.
 * Not advised, but it can be done.
 */
#define MEMORY 1 /* 0 or 1 to delegate malloc'd memory or cast unsigned long to void* */

static unsigned int Goal; /* use how far to go to size the stores */

static void *
primeT(
  void *v
){
#if MEMORY
  unsigned int *ip;
#else
  unsigned long ip;
#endif
  chan_t *c;
  pthread_t t;
  unsigned int prime;
#if STORE
  unsigned int i;
#endif

  pthread_cleanup_push((void(*)(void*))chanClose, v);
  if (chanGet(-1, v, (void **)&ip) != chanOsGet)
    goto exit0;
#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  c = 0;
  pthread_cleanup_push((void(*)(void*))chanClose, c);
#if STORE
  if ((i = (Goal - prime) / 100) > 1)
    c = chanCreate(chanFifoSi, chanFifoSa(i, i), chanFifoSd);
  else
#endif
    c = chanCreate(0,0,0);
  if (!c) {
    puts("Can't create more channels, draining pipeline...");
    goto drain;
  }
  chanOpen(c);
  if (pthread_create(&t, 0, primeT, c)) {
    chanClose(c);
    puts("Can't create more threads, draining pipeline...");
drain:
    while (chanGet(-1, v, (void **)&ip) == chanOsGet)
#if MEMORY
      free(ip)
#endif
      ;
    goto exit1;
  }
  for (;;) {
    if (chanGet(-1, v, (void **)&ip) != chanOsGet)
      break;
#if MEMORY
    if (*ip % prime)
#else
    if (ip % prime)
#endif
    {
      chanOs_t r;

#if MEMORY
      r = chanPut(-1, c, ip);
#else
      r = chanPut(-1, c, (void *)ip);
#endif
      if (r != chanOsPut)
        break;
    }
#if MEMORY
      else
        free(ip);
#endif
  }
  chanShut(c);
  pthread_join(t, 0);
  printf("joined %d\n", prime); fflush(stdout);
exit1:
  pthread_cleanup_pop(1); /* chanClose(c) */
exit0:
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

int
main(
  int argc
 ,char **argv
){
  chan_t *c;
  pthread_t t;
#if MEMORY
    unsigned int *ip;
#else
    unsigned long ip;
#endif
  unsigned int i;

  if (argc < 2
   || (Goal = atoi(*(argv + 1))) < 2)
    Goal = 100;
  printf("Goal = %d\n", Goal);
  c = 0;
  pthread_cleanup_push((void(*)(void*))chanClose, c);
#if STORE
  if ((i = (Goal - 2) / 100) > 1)
    c = chanCreate(chanFifoSi, chanFifoSa(i, i), chanFifoSd);
  else
#endif
    c = chanCreate(0,0,0);
  if (!c) {
    puts("Can't create channel");
    goto exit0;
  }
  chanOpen(c);
  if (pthread_create(&t, 0, primeT, c)) {
    chanClose(c);
    puts("Can't create thread");
    goto exit0;
  }
  puts("2");
  for (i = 3; i <= Goal; i += 2) {
    chanOs_t r;

#if MEMORY
    if (!(ip = malloc(sizeof (*ip)))) {
      puts("Can't malloc");
      break;
    }
    *ip = i;
    r = chanPut(-1, c, ip);
#else
    ip = i;
    r = chanPut(-1, c, (void *)ip);
#endif
    if (r != chanOsPut)
      break;
  }
  chanShut(c);
  pthread_join(t, 0);
  printf("joined Goal\n"); fflush(stdout);
exit0:
  pthread_cleanup_pop(1); /* chanClose(c) */
  return (0);
}
