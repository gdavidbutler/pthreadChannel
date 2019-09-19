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

#define POLL 1 /* 0 or 1 to use chanPoll */

unsigned int Goal; /* use how far to go to size the stores */

#if POLL

void *
primeT(
  void *v
){
#if MEMORY
  unsigned int *ip;
#else
  unsigned long ip;
#endif
  chanPoll_t c[2]; /* 0 is the read channel and 1 is the write channel */
  pthread_t t;
  unsigned int prime;

  chanOpen(v);
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  c[0].c = v;
  c[0].v = (void **)&ip;
  c[0].o = chanOpPull;
  c[1].c = 0;
  c[1].v = (void **)&ip;
  c[1].o = chanOpNoOp;
  if (!chanPoll(0, sizeof(c) / sizeof(c[0]), c))
    goto exit0;
#if STORE
#if MEMORY
  if ((prime = (Goal - *ip) / 10) > 1)
#else
  if ((prime = (Goal - ip) / 10) > 1)
#endif
    c[1].c = chanCreate(realloc, free, chanFifoSi, chanFifoSa(realloc, free, prime), chanFifoSd);
  else
#endif
    c[1].c = chanCreate(realloc, free, 0, 0, 0);
  pthread_cleanup_push((void(*)(void*))chanClose, c[1].c);
  if (!c[1].c) {
    puts("Can't create more channels, draining pipeline...");
    goto drain;
  }
#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  if (pthread_create(&t, 0, primeT, c[1].c)) {
    puts("Can't create more threads, draining pipeline...");
drain:
    while (chanPoll(0, sizeof(c) / sizeof(c[0]), c))
#if MEMORY
      free(ip)
#endif
      ;
    goto exit1;
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
        c[0].o = chanOpNoOp;
        c[1].o = chanOpPush;
      }
#if MEMORY
      else
        free(ip);
#endif
      break;
    case 2:
      c[0].o = chanOpPull;
      c[1].o = chanOpNoOp;
      break;
    default:
      goto endFor;
      break;
    }
  }
endFor:
  chanShut(c[1].c);
  pthread_join(t, 0);
  printf("joined %d\n", prime); fflush(stdout);
exit1:
  pthread_cleanup_pop(1); /* chanClose(c[1].c) */
exit0:
  pthread_cleanup_pop(1); /* chanClose(v) */
  return 0;
}

int
main(
  int argc
 ,char **argv
){
  chanPoll_t h[1];
  pthread_t t;
  unsigned int i;
#if MEMORY
    unsigned int *ip;
#else
    unsigned long ip;
#endif

  if (argc < 2
   || (Goal = atoi(*(argv + 1))) < 2)
    Goal = 100;
  printf("Goal = %d\n", Goal);
#if STORE
  if ((i = Goal / 10) > 1)
    h[0].c = chanCreate(realloc, free, chanFifoSi, chanFifoSa(realloc, free, i), chanFifoSd);
  else
#endif
    h[0].c = chanCreate(realloc, free, 0, 0, 0);
  if (!h[0].c) {
    puts("Can't create channel");
    return 0;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, h[0].c);
  h[0].v = (void **)&ip;
  h[0].o = chanOpPush;
  if (pthread_create(&t, 0, primeT, h[0].c)) {
    puts("Can't create thread");
    return 0;
  }
  puts("2");
  for (i = 3; i <= Goal; i += 2) {
#if MEMORY
    if (!(ip = malloc(sizeof(*ip)))) {
      puts("Can't malloc");
      break;
    }
    *ip = i;
#else
    ip = i;
#endif
    if (!chanPoll(0, sizeof(h) / sizeof(h[0]), h))
      break;
  }
  chanShut(h[0].c);
  pthread_join(t, 0);
  printf("joined Goal\n"); fflush(stdout);
  pthread_cleanup_pop(1); /* chanClose(h[0].c) */
  return 0;
}

#else /* POLL */

void *
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

  chanOpen(v);
  pthread_cleanup_push((void(*)(void*))chanClose, v);
  if (!chanPull(0, v, (void **)&ip))
    goto exit0;
#if STORE
#if MEMORY
  if ((prime = (Goal - *ip) / 10) > 1)
#else
  if ((prime = (Goal - ip) / 10) > 1)
#endif
    c = chanCreate(realloc, free, chanFifoSi, chanFifoSa(realloc, free, prime), chanFifoSd);
  else
#endif
    c = chanCreate(realloc, free, 0, 0, 0);
  pthread_cleanup_push((void(*)(void*))chanClose, c);
  if (!c) {
    puts("Can't create more channels, draining pipeline...");
    goto drain;
  }
#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  if (pthread_create(&t, 0, primeT, c)) {
drain:
    puts("Can't create more threads, draining pipeline...");
    while (chanPull(0, v, (void **)&ip))
#if MEMORY
      free(ip)
#endif
      ;
    goto exit1;
  }
  for (;;) {
    if (!chanPull(0, v, (void **)&ip))
      break;
#if MEMORY
    if (*ip % prime)
#else
    if (ip % prime)
#endif
    {
      int r;

#if MEMORY
      r = chanPush(0, c, ip);
#else
      r = chanPush(0, c, (void *)ip);
#endif
      if (!r)
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
#if MEMORY
    unsigned int *ip;
#else
    unsigned long ip;
#endif

  if (argc < 2
   || (Goal = atoi(*(argv + 1))) < 2)
    Goal = 100;
  printf("Goal = %d\n", Goal);
#if STORE
  if ((i = Goal / 10) > 1)
    c = chanCreate(realloc, free, chanFifoSi, chanFifoSa(realloc, free, i), chanFifoSd);
  else
#endif
    c = chanCreate(realloc, free, 0, 0, 0);
  if (!c) {
    puts("Can't create channel");
    return 0;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, c);
  if (pthread_create(&t, 0, primeT, c)) {
    puts("Can't create thread");
    return 0;
  }
  puts("2");
  for (i = 3; i <= Goal; i += 2) {
    int r;

#if MEMORY
    if (!(ip = malloc(sizeof(*ip)))) {
      puts("Can't malloc");
      break;
    }
    *ip = i;
    r = chanPush(0, c, ip);
#else
    ip = i;
    r = chanPush(0, c, (void *)ip);
#endif
    if (!r)
      break;
  }
  chanShut(c);
  pthread_join(t, 0);
  printf("joined Goal\n"); fflush(stdout);
  pthread_cleanup_pop(1); /* chanClose(c) */
  return 0;
}

#endif /* POLL */
