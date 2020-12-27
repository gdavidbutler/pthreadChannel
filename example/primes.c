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

/* Based on https://swtch.com/libtask/primes.c */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
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
 * Since this example talks more and works less (see README, chan.h and chanFifo.h),
 * using a FIFO store drastically decreases thread context switching.
 * Since each prime thread is a filter, there are many more messages at the head of
 * the chain than at the end. Stores are sized relative to the length of the chain.
 * The context switch overhead dominates, so the initial size is set to the max.
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

static unsigned int Goal; /* use how far to go to size the stores, could be passed a as parameter to each thread */

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

  prime = 0;
  c = 0;
  if (chanOp(0, v, (void **)&ip, chanOpGet) != chanOsGet) {
    puts("out of memory");
    goto exit;
  }

#if MEMORY
  prime = *ip;
  free(ip);
#else
  prime = ip;
#endif
  printf("%d\n", prime);
  if (prime > Goal)
    goto exit;

#if STORE
  if ((i = (Goal - prime) / 500) > 1) {
    void *tv;

    if (!(tv = chanFifoDySa(i, i / 2))
     || !(c = chanCreate(chanFifoDySi, tv, chanFifoDySd)))
      free(tv);
  } else
#endif
    c = chanCreate(0,0,0);
  if (!c) {
    puts("out of memory");
    goto exit;
  }

  chanOpen(c);
  if (pthread_create(&t, 0, primeT, c)) {
    chanClose(c);
    puts("out of threads");
    goto exit;
  }
  pthread_detach(t);

  while (chanOp(0, v, (void **)&ip, chanOpGet) == chanOsGet) {
#if MEMORY
    if (*ip % prime) {
      if (chanOp(0, c, (void **)&ip, chanOpPut) != chanOsPut) {
        free(ip);
        break;
      }
    } else
      free(ip);
#else
    if (ip % prime && chanOp(0, c, (void **)&ip, chanOpPut) != chanOsPut)
      break;
#endif
  }

exit:
  printf("%d done\n", prime);
  chanShut(c);
  chanClose(c);
  chanShut(v);
  while (chanOp(0, v, (void **)&ip, chanOpGet) == chanOsGet)
#if MEMORY
    free(ip)
#endif
    ;
  chanClose(v);
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

  chanInit(realloc, free);
  if (argc < 2
   || (Goal = atoi(*(argv + 1))) < 2)
    Goal = 100;
  printf("Goal = %d\n", Goal);

#if STORE
  if ((i = (Goal - 2) / 500) > 1) {
    void *tv;

    if (!(tv = chanFifoDySa(i, i / 2))
     || !(c = chanCreate(chanFifoDySi, tv, chanFifoDySd))) {
      free(tv);
      c = 0;
    }
  } else
#endif
  if (!(c = chanCreate(0,0,0))) {
    puts("Can't create channel");
    goto exit;
  }

  chanOpen(c);
  if (pthread_create(&t, 0, primeT, c)) {
    chanClose(c);
    puts("Can't create thread");
    goto exit;
  }
  pthread_detach(t);

  puts("2");
  for (i = 3; i; i += 2) {
#if MEMORY
    if (!(ip = malloc(sizeof (*ip)))) {
      puts("Can't malloc");
      break;
    }
    *ip = i;
    if (chanOp(0, c, (void **)&ip, chanOpPut) != chanOsPut) {
      free(ip);
      break;
    }
#else
    ip = i;
    if (chanOp(0, c, (void **)&ip, chanOpPut) != chanOsPut)
      break;
#endif
  }
  puts("2 done");
exit:
  chanShut(c);
  chanClose(c);
  return (0);
}
