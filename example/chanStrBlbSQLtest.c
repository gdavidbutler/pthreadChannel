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

/*
 * compile SQLite with something like:
 *  cc -std=c99 -I. -Isqlite-amalgamation-3470000 -Os -g -c -DSQLITE_OMIT_DEPRECATED
 *   -DSQLITE_OMIT_AUTOINIT -DSQLITE_OMIT_AUTORESET -DSQLITE_OMIT_PROGRESS_CALLBACK
 *   -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_TEMP_STORE=2 -DSQLITE_DEFAULT_MEMSTATUS=0
 *   -DSQLITE_DEFAULT_DEFENSIVE=1 -DSQLITE_DQS=0 -DSQLITE_TRUSTED_SCHEMA=0
 *   -DSQLITE_THREADSAFE=2 sqlite-amalgamation-3470000/sqlite3.c
 *
 * complile this program with something like:
 *  cc -std=c99 -I. -Iexample -Isqlite-amalgamation-3470000 -Os -g -c example/chanStrBlbSQL.c
 *  cc -std=c99 -I. -Iexample -Isqlite-amalgamation-3470000 -Os -g -c example/chanStrBlbSQLtest.c
 *
 * then link it all together with the channel objects like:
 *  cc -Os -g -o chanStrBlbSQLtest chanStrBlbSQLtest.o chanStrBlbSQL.o chan.o chanBlb.o sqlite3.o -lpthread
 *
 * run it like:
 *  (file for SQLite, :memory: is fast but much more expensive than chanStrFIFO)
 *  (message limit is only significant on first create)
 *  (SQLite PRAGMA journal_mode, 0=DELETE, 1=TRUNCATE, 2=PERSIST)
 *  (SQLite PRAGMA synchronous, 0=OFF, 1=NORMAL, 2=FULL, 3=EXTRA)
 *  (g: get or p: put or b: both get and put)
 *
 *  ./chanStrBlbSQL chanStrBlbSQL.db 2 0 100 p < README.md
 *   chanStrBlbSQL.db is full of README.md blobs
 *
 *  ./chanStrBlbSQL chanStrBlbSQL.db 2 0 1 g > README.out
 *   chanStrBlbSQL.db is empty
 *
 *  cmp README.md README.out
 *
 * or both:
 *
 *  ./chanStrBlbSQL chanStrBlbSQL.db 2 0 100 b < README.md > README.out
 *   chanStrBlbSQL.db is empty
 *
 *  cmp README.md README.out
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "sqlite3.h"
#include "chanStrBlbSQL.h"

/* input thread for get */
static void *
inT(
 void *v
){
  chanBlb_t *m;
  int i;

  pthread_cleanup_push((void(*)(void*))chanClose, v);
  while ((m = malloc(chanBlb_tSize(BUFSIZ)))
   && (i = read(0, m->b, BUFSIZ)) > 0) {
    void *t;

    m->l = i;
    if ((t = realloc(m, chanBlb_tSize(m->l))))
      m = t;
    if (chanOp(0, v, (void **)&m, chanOpPut) != chanOsPut)
      break;
  }
  free(m);
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

/* output thread for put */
static void *
outT(
 void *v
){
  chanBlb_t *m;

  pthread_cleanup_push((void(*)(void*))chanClose, v);
  while (chanOp(0, v, (void **)&m, chanOpGet) == chanOsGet) {
    unsigned int l;
    int i;

    for (l = 0; l < m->l && (i = write(1, m->b + l, m->l - l)) > 0; l += i);
    free(m);
  }
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}


int
main(
  int argc
 ,char *argv[]
){
  chanStrBlbSQLc_t *x;
  chan_t *c;
  pthread_t in;
  pthread_t out;
  sqlite3_int64 z;
  int j;
  int s;

  if (argc < 6
   || !*argv[1]
   || ((j = atoi(argv[2])) < 0 || j > 3)
   || ((s = atoi(argv[3])) < 0 || s > 2)
   || (z = atoll(argv[4])) <= 0
   || (*argv[5] != 'g' && *argv[5] != 'p' && *argv[5] != 'b')) {
    fprintf(stderr, "Usage: %s file synchronous messages g|p|b\n", argv[0]);
    return (1);
  }
  sqlite3_initialize();
  if (!(j = chanStrBlbSQLa(&x, argv[1], j, s, z))) {
    perror("chanStrBlbSQLa");
    return (1);
  }
  chanInit(realloc, free);
  if (!(c = chanCreate(x, (chanSd_t)chanStrBlbSQLd, (chanSi_t)chanStrBlbSQLi, j))) {
    chanStrBlbSQLd(x, 0);
    perror("chanCreate");
    return (1);
  }
  if (*argv[5] != 'g') {
    chanOpen(c);
    if (pthread_create(&in, 0, inT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[5] != 'p') {
    chanOpen(c);
    if (pthread_create(&out, 0, outT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[5] != 'g')
    pthread_join(in, 0);
  chanShut(c);
  chanClose(c);
  if (*argv[5] != 'p')
    pthread_join(out, 0);
  sqlite3_shutdown();
  return (0);
}
