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
 * (SQLITE_THREADSAFE can be anything as the single connection is guarded with a mutex.)
 *
 *  cc -std=c99 -Isqlite-amalgamation-??????? -DSQLITE_THREADSAFE=0 -Os -g -c sqlite-amalgamation-???????/sqlite3.c
 *
 * compile example with something like:
 *
 *  cc -std=c99 -I. -Iexample -Isqlite-amalgamation-??????? -DSQLITE_THREADSAFE=0 -Os -g -c example/chanStrBlbSQL.c
 *  cc -std=c99 -I. -Iexample -Isqlite-amalgamation-??????? -DSQLITE_THREADSAFE=0 -Os -g -c example/chanStrBlbSQLtest.c
 *
 * link it all together with channel objects like:
 *
 *  cc -g -o chanStrBlbSQLtest chanStrBlbSQLtest.o chanStrBlbSQL.o sqlite3.o chanBlb.o chanStrFIFO.o chan.o -lpthread
 *
 * Usage:
 *  ./chanStrBlbSQLtest file journal(0:DELETE,1:TRUNCATE,2:PERSIST,3:WAL) synchronous(0:OFF,1:NORMAL,2:FULL,3:EXTRA) messages g|p|b
 *
 *  (file for SQLite, :memory: is fast but much more expensive than chanStrFIFO, which is used if file is zero length)
 *  (SQLite PRAGMA journal_mode, 0=DELETE, 1=TRUNCATE, 2=PERSIST, 3=WAL)
 *  (SQLite PRAGMA synchronous, 0=OFF, 1=NORMAL, 2=FULL, 3=EXTRA)
 *  (message limit, only significant on initial create)
 *  (g:get or p:put or b:both get and put)
 *
 * run it like:
 *
 *  ./chanStrBlbSQLtest test.db 0 2 100 p < README.md
 *   test.db is full of README.md blobs
 *
 *  ./chanStrBlbSQLtest test.db 0 2 100 g > README.out
 *   test.db is empty
 *
 *  cmp README.md README.out
 *
 * or both in one process:
 *
 *  ./chanStrBlbSQLtest test.db 0 2 100 b < README.md > README.out
 *   test.db is empty
 *
 *  cmp README.md README.out
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "sqlite3.h"
#include "chanStrFIFO.h"
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
    if (i < BUFSIZ && (t = realloc(m, chanBlb_tSize(m->l))))
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
  enum {
   arg_prog
  ,arg_file
  ,arg_journal
  ,arg_synchronous
  ,arg_messages
  ,arg_operation
  ,arg_NUM
  };
  chan_t *c;
  pthread_t in;
  pthread_t out;
  sqlite3_int64 z;
  int j;
  int s;

  if (argc < arg_NUM
   || ((j = atoi(argv[arg_journal])) < 0 || j > 3)
   || ((s = atoi(argv[arg_synchronous])) < 0 || s > 3)
   || (z = atoll(argv[arg_messages])) <= 0
   || (*argv[arg_operation] != 'g' && *argv[arg_operation] != 'p' && *argv[arg_operation] != 'b')
   || (!*argv[arg_file] && *argv[arg_operation] != 'b')) {
    fprintf(stderr, "Usage: %s"
                    " file"
                    " journal(0:DELETE,1:TRUNCATE,2:PERSIST,3:WAL)"
                    " synchronous(0:OFF,1:NORMAL,2:FULL,3:EXTRA)"
                    " messages"
                    " g|p|b\n"
                    "\n", argv[arg_prog]);
    return (1);
  }
  chanInit(realloc, free);
  sqlite3_initialize();
  if (*argv[arg_file])
    c = chanCreate(free, chanStrBlbSQLa, malloc, argv[arg_file], j, s, z);
  else
    c = chanCreate(free, chanStrFIFOa, z);
  if (!c) {
    perror("chanCreate");
    return (1);
  }
  if (*argv[arg_operation] != 'g') {
    chanOpen(c);
    if (pthread_create(&in, 0, inT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[arg_operation] != 'p') {
    chanOpen(c);
    if (pthread_create(&out, 0, outT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[arg_operation] != 'g')
    pthread_join(in, 0);
  chanShut(c);
  if (*argv[arg_operation] != 'p')
    pthread_join(out, 0);
  chanClose(c);
  sqlite3_shutdown();
  return (0);
}
