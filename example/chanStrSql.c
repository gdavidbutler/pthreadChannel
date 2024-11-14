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
 *  cc -std=c99 -I. -Isqlite-amalgamation-3470000 -Os -g -c example/chanStrSql.c
 *
 * then link it all together with the channel objects like:
 *  cc -Os -g -o chanStrSql chanStrSql.o chan.o chanBlb.o sqlite3.o -lpthread
 *
 * run it like (g: get or p: put or b: both get and put) (message limit is only significant on first create):
 *  ./chanStrSql p chanStrSql.db 100 < README.md
 *   chanStrSql.db is full of README.md blobs
 *
 *  ./chanStrSql g chanStrSql.db 1 > README.out
 *   chanStrSql.db is empty
 *
 *  cmp README.md README.out
 *
 * or both
 *  ./chanStrSql b chanStrSql.db 100 < README.md > README.out
 *   chanStrSql.db is empty
 *
 *  cmp README.md README.out
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "sqlite3.h"

/* channel Store FIFO Context */
struct sqlSc {
  sqlite3 *d;
  sqlite3_stmt *bgn;
  sqlite3_stmt *cmt;
  sqlite3_stmt *rlb;
  sqlite3_stmt *insB;
  sqlite3_stmt *updT;
  sqlite3_stmt *delB;
  sqlite3_stmt *updH;
  sqlite3_stmt *rstH;
};

/* channel Store FIFO Deallocation */
static void
sqlSd(
  struct sqlSc *c
 ,chanSs_t s
){
  if (!c)
    return;
  sqlite3_finalize(c->bgn);
  sqlite3_finalize(c->cmt);
  sqlite3_finalize(c->rlb);
  sqlite3_finalize(c->insB);
  sqlite3_finalize(c->updT);
  sqlite3_finalize(c->delB);
  sqlite3_finalize(c->updH);
  sqlite3_finalize(c->rstH);
  sqlite3_exec(c->d, "PRAGMA journal_mode=DELETE;", 0, 0, 0);
  sqlite3_close(c->d);
  sqlite3_free(c);
  return;
  (void)s;
}

/* channel Store FIFO Implementation */
static chanSs_t
sqlSi(
  struct sqlSc *c
 ,chanSo_t o
 ,chanSw_t w
 ,chanBlb_t **v
){
  const void *t;
  int i;

  if (!c)
    return (0);
  sqlite3_step(c->bgn), sqlite3_reset(c->bgn);
  if (o == chanSoPut) {
    sqlite3_bind_blob(c->insB, 1, (*v)->b, (*v)->l, SQLITE_STATIC);
    if (sqlite3_step(c->insB) != SQLITE_DONE)
      goto err;
    sqlite3_reset(c->insB);
    free(*v);
    if (sqlite3_step(c->updT) != SQLITE_ROW)
      goto err;
    i = sqlite3_column_int(c->updT, 0);
    sqlite3_reset(c->updT);
    sqlite3_step(c->cmt), sqlite3_reset(c->cmt);
    if (i)
      return (chanSsCanGet);
  } else {
    if (sqlite3_step(c->delB) != SQLITE_ROW
     || !(t = sqlite3_column_blob(c->delB, 0))
     || (i = sqlite3_column_bytes(c->delB, 0)) < 0
     || !(*v = malloc(chanBlb_tSize(i)))
    )
      goto err;
    (*v)->l = i;
    memcpy((*v)->b, t, i);
    sqlite3_reset(c->delB);
    if (sqlite3_step(c->updH) != SQLITE_ROW)
      goto err;
    i = sqlite3_column_int(c->updH, 0);
    sqlite3_reset(c->updH);
    if (i) {
      if (sqlite3_step(c->rstH) != SQLITE_DONE)
        goto err;
      sqlite3_reset(c->rstH);
      sqlite3_step(c->cmt), sqlite3_reset(c->cmt);
      return (chanSsCanPut);
    } else
      sqlite3_step(c->cmt), sqlite3_reset(c->cmt);
  }
  return (chanSsCanGet | chanSsCanPut);
err:
  sqlite3_step(c->rlb), sqlite3_reset(c->rlb);
  return (0);
  (void)w;
}

/* channel Store FIFO Allocation */
static chanSs_t
sqlSa(
  struct sqlSc **c
 ,const char *p
 ,sqlite3_int64 z
){
  sqlite3_stmt *s;
  int i;

  if (!c)
    return (0);
  if (!p || !z) {
    *c = 0;
    return (0);
  }
  if (!(*c = sqlite3_malloc(sizeof (**c))))
    return (0);
  memset(*c, 0, sizeof (**c));
  if (sqlite3_open_v2(p, &(*c)->d, SQLITE_OPEN_READWRITE, 0)) {
    sqlite3_close((*c)->d);
    if (sqlite3_open_v2(p, &(*c)->d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0))
      return (0);
    if (sqlite3_exec((*c)->d
     ,"BEGIN;"
      "CREATE TABLE \"H\"("
      "\"i\" INTEGER PRIMARY KEY"
      ",\"l\" INTEGER" /* limit */
      ",\"h\" INTEGER" /* head */
      ",\"t\" INTEGER" /* tail */
      ");"
      "CREATE TABLE \"B\"("
      "\"i\" INTEGER PRIMARY KEY"
      ",\"b\" BLOB"
      ");"
      "COMMIT;"
     ,0, 0, 0))
      goto err;
    if (sqlite3_prepare_v2((*c)->d
     ,"INSERT INTO \"H\" VALUES (1,?1,1,1);"
     ,-1, &s, 0))
      goto err;
    sqlite3_bind_int64(s, 1, z); /* largest signed 64bit = 9223372036854775807 */
    if (sqlite3_step(s) != SQLITE_DONE)
      goto err;
    sqlite3_finalize(s);
  }
  if (sqlite3_exec((*c)->d
   ,"PRAGMA locking_mode=EXCLUSIVE;"
    "PRAGMA journal_mode=PERSIST;"
    "PRAGMA synchronous=NORMAL;" /* no directory changes with PERSIST */
   ,0, 0, 0))
    goto err;
  if (sqlite3_prepare_v3((*c)->d
   ,"BEGIN"
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->bgn, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"COMMIT"
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->cmt, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"ROLLBACK"
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->rlb, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"INSERT INTO \"B\" VALUES((SELECT \"t\" FROM \"H\" WHERE \"i\"=1),?1)"
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->insB, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"UPDATE \"H\" SET \"t\"=CASE WHEN \"t\"=\"l\" THEN 1 ELSE \"t\"+1 END WHERE \"i\"=1 RETURNING \"t\"=\"h\""
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->updT, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"DELETE FROM \"B\" WHERE \"i\"=(SELECT \"h\" FROM \"H\" WHERE \"i\"=1) RETURNING \"b\""
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->delB, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"UPDATE \"H\" SET \"h\"=CASE WHEN \"h\"=\"l\" THEN 1 ELSE \"h\"+1 END WHERE \"i\"=1 RETURNING \"h\"=\"t\""
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->updH, 0)
   || sqlite3_prepare_v3((*c)->d
   ,"UPDATE \"H\" SET \"h\"=1, \"t\"=1 WHERE \"i\"=1"
   ,-1, SQLITE_PREPARE_PERSISTENT, &(*c)->rstH, 0)
   || sqlite3_prepare_v2((*c)->d
   ,"SELECT (SELECT COUNT(*) FROM \"B\")>0, \"h\"=\"t\" FROM \"H\" WHERE \"i\"=1"
   ,-1, &s, 0)
  )
    goto err;
  if (sqlite3_step(s) != SQLITE_ROW)
    goto err;
  i = 0;
  if (sqlite3_column_int(s, 0)) /* not empty */
    i |= chanSsCanGet;
  if (!sqlite3_column_int(s, 0) || !sqlite3_column_int(s, 1)) /* not full */
    i |= chanSsCanPut;
  sqlite3_finalize(s);
  return (i);
err:
  sqlSd((*c), 0);
  return (0);
}

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
  struct sqlSc *sqlSc;
  chan_t *c;
  pthread_t in;
  pthread_t out;
  sqlite3_int64 sz;
  int i;

  if (argc < 4
   || (*argv[1] != 'g' && *argv[1] != 'p' && *argv[1] != 'b')
   || !*argv[2]
   || (sz = atoll(argv[3])) <= 0) {
    fprintf(stderr, "Usage: %s g|p|b file messages\n", argv[0]);
    return (1);
  }
  sqlite3_initialize();
  if (!(i = sqlSa(&sqlSc, argv[2], sz))) {
    perror("sqlSa");
    return (1);
  }
  chanInit(realloc, free);
  if (!(c = chanCreate(sqlSc, (chanSd_t)sqlSd, (chanSi_t)sqlSi, i))) {
    sqlSd(sqlSc, 0);
    perror("chanCreate");
    return (1);
  }
  if (*argv[1] != 'g') {
    chanOpen(c);
    if (pthread_create(&in, 0, inT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[1] != 'p') {
    chanOpen(c);
    if (pthread_create(&out, 0, outT, c)) {
      chanClose(c);
      chanShut(c);
      chanClose(c);
      perror("pthread_create");
      return (1);
    }
  }
  if (*argv[1] != 'g')
    pthread_join(in, 0);
  chanShut(c);
  chanClose(c);
  if (*argv[1] != 'p')
    pthread_join(out, 0);
  sqlite3_shutdown();
  return (0);
}
