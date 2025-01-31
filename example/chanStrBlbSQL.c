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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "sqlite3.h"
#include "chanStrBlbSQL.h"

struct chanStrBlbSQLc {
  void (*f)(void *);          /* infrastructure free routine */
  void (*d)(void *);          /* blob delete routine */
  void *(*n)(unsigned long);  /* blob new routine */
  sqlite3 *b;                 /* connection */
  sqlite3_stmt *bgn;
  sqlite3_stmt *cmt;
  sqlite3_stmt *rlb;
  sqlite3_stmt *insB;
  sqlite3_stmt *updT;
  sqlite3_stmt *delB;
  sqlite3_stmt *updH;
  sqlite3_stmt *sel;
  pthread_mutex_t m;          /* mutext to cover use of the connection */
};

#define C ((struct chanStrBlbSQLc *)c)

static void
chanStrBlbSQLd(
  void *c
 ,chanSs_t s
){
  if (!c)
    return;
  pthread_mutex_lock(&C->m);
  sqlite3_finalize(C->bgn);
  sqlite3_finalize(C->cmt);
  sqlite3_finalize(C->rlb);
  sqlite3_finalize(C->insB);
  sqlite3_finalize(C->updT);
  sqlite3_finalize(C->delB);
  sqlite3_finalize(C->updH);
  sqlite3_finalize(C->sel);
  sqlite3_close(C->b);
  pthread_mutex_unlock(&C->m);
  pthread_mutex_destroy(&C->m);
  C->f(c);
  return;
  (void)s; /* leaving blobs in store */
}

#define V ((chanBlb_t **)v)

static chanSs_t
chanStrBlbSQLi(
  void *c
 ,chanSo_t o
 ,chanSw_t w
 ,void **v
){
  const void *t;
  int i;

  if (!c)
    return (0);
  pthread_mutex_lock(&C->m);
  sqlite3_step(C->bgn), sqlite3_reset(C->bgn);
  if (o == chanSoPut) {
    sqlite3_bind_blob(C->insB, 1, (*V)->b, (*V)->l, SQLITE_STATIC);
    if (sqlite3_step(C->insB) != SQLITE_DONE)
      goto err;
    sqlite3_reset(C->insB);
    C->d(*v);
    if (sqlite3_step(C->updT) != SQLITE_ROW)
      goto err;
    i = sqlite3_column_int(C->updT, 0);
    sqlite3_reset(C->updT);
    sqlite3_step(C->cmt), sqlite3_reset(C->cmt);
    if (i) {
      pthread_mutex_unlock(&C->m);
      return (chanSsCanGet);
    }
  } else {
    if (sqlite3_step(C->delB) != SQLITE_ROW
     || !(t = sqlite3_column_blob(C->delB, 0))
     || (i = sqlite3_column_bytes(C->delB, 0)) < 0
     || !(*v = C->n(chanBlb_tSize(i)))
    )
      goto err;
    (*V)->l = i;
    memcpy((*V)->b, t, i);
    sqlite3_reset(C->delB);
    if (sqlite3_step(C->updH) != SQLITE_ROW)
      goto err;
    i = sqlite3_column_int(C->updH, 0);
    sqlite3_reset(C->updH);
    sqlite3_step(C->cmt), sqlite3_reset(C->cmt);
    if (i) {
      pthread_mutex_unlock(&C->m);
      return (chanSsCanPut);
    }
  }
  pthread_mutex_unlock(&C->m);
  return (chanSsCanGet | chanSsCanPut);
err:
  sqlite3_reset(C->insB);
  sqlite3_reset(C->updT);
  sqlite3_reset(C->delB);
  sqlite3_reset(C->updH);
  sqlite3_step(C->rlb), sqlite3_reset(C->rlb);
  pthread_mutex_unlock(&C->m);
  return (0);
  (void)w; /* not concerned with latency */
}

#undef V

#undef C

chanSs_t
chanStrBlbSQLa(
  void *(*a)(void *, unsigned long)
 ,void (*f)(void *)
 ,void (*u)(void *)
 ,int (*w)(void *, chanSs_t)
 ,void *x
 ,chanSd_t *d
 ,chanSi_t *i
 ,void **v
 ,va_list l
){
  static const char *jnl[] = {
    "PRAGMA journal_mode=DELETE;"
   ,"PRAGMA journal_mode=TRUNCATE;"
   ,"PRAGMA journal_mode=PERSIST;"
   ,"PRAGMA journal_mode=WAL;"
  };
  static const char *syn[] = {
    "PRAGMA synchronous=OFF;"
   ,"PRAGMA synchronous=NORMAL;"
   ,"PRAGMA synchronous=FULL;"
   ,"PRAGMA synchronous=EXTRA;"
  };
  struct chanStrBlbSQLc *c;
  void *(*n)(unsigned long);
  const char *p;
  unsigned int o;
  unsigned int y;
  sqlite3_int64 z;
  int s;

  if (!v)
    return (0);
  *v = 0;
  n = va_arg(l, void *(*)(unsigned long)); /* blob new routine */
  p = va_arg(l, const char *);             /* sqlite path name */
  o = va_arg(l, unsigned int);             /* journal_mode */
  y = va_arg(l, unsigned int);             /* synchronous */
  z = va_arg(l, sqlite3_int64);            /* size to allow */
  if (!a || !f || !p
   || o >= sizeof (jnl) / sizeof (jnl[0])
   || y >= sizeof (syn) / sizeof (syn[0])
   || !z)
    return (0);
  if (!(c = a(0, sizeof (*c))))
    return (0);
  memset(c, 0, sizeof (*c));
  if (pthread_mutex_init(&c->m, 0)) {
    f(c);
    return (0);
  }
  c->f = f;
  c->d = u;
  c->n = n;
  if (sqlite3_open_v2(p, &c->b, SQLITE_OPEN_READWRITE, 0)) {
    sqlite3_close(c->b);
    if (sqlite3_open_v2(p, &c->b, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0))
      goto err;
  }
  if (sqlite3_exec(c->b, "PRAGMA locking_mode=EXCLUSIVE;", 0, 0, 0)
   || sqlite3_exec(c->b, jnl[o], 0, 0, 0)
   || sqlite3_exec(c->b, syn[y], 0, 0, 0))
    goto err;
  if (sqlite3_exec(c->b
   ,"BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS \"H\"("
    "\"i\" INTEGER PRIMARY KEY"
    ",\"l\" INTEGER" /* limit */
    ",\"h\" INTEGER" /* head */
    ",\"t\" INTEGER" /* tail */
    ");"
    "CREATE TABLE IF NOT EXISTS \"B\"("
    "\"i\" INTEGER PRIMARY KEY"
    ",\"b\" BLOB"
    ");"
   ,0, 0, 0))
    goto err;
  if (sqlite3_prepare_v2(c->b
   ,"INSERT OR IGNORE INTO \"H\" VALUES (1,?1,1,1);"
   ,-1, &c->bgn, 0))
    goto err;
  sqlite3_bind_int64(c->bgn, 1, z); /* largest signed 64bit = 9223372036854775807 */
  if (sqlite3_step(c->bgn) != SQLITE_DONE)
    goto err;
  sqlite3_finalize(c->bgn);
  if (sqlite3_prepare_v3(c->b
    ,"BEGIN IMMEDIATE"
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->bgn, 0)
   || sqlite3_prepare_v3(c->b
    ,"COMMIT"
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->cmt, 0)
   || sqlite3_prepare_v3(c->b
    ,"ROLLBACK"
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->rlb, 0)
   || sqlite3_prepare_v3(c->b
    ,"INSERT INTO \"B\" VALUES((SELECT \"t\" FROM \"H\" WHERE \"i\"=1),?1)"
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->insB, 0)
   || sqlite3_prepare_v3(c->b
    ,"UPDATE \"H\" SET \"t\"=CASE WHEN \"t\"=\"l\" THEN 1 ELSE \"t\"+1 END WHERE \"i\"=1 RETURNING \"t\"=\"h\""
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->updT, 0)
   || sqlite3_prepare_v3(c->b
    ,"DELETE FROM \"B\" WHERE \"i\"=(SELECT \"h\" FROM \"H\" WHERE \"i\"=1) RETURNING \"b\""
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->delB, 0)
   || sqlite3_prepare_v3(c->b
    ,"UPDATE \"H\" SET \"h\"=CASE WHEN \"h\"=\"l\" THEN 1 ELSE \"h\"+1 END WHERE \"i\"=1 RETURNING \"h\"=\"t\""
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->updH, 0)
   || sqlite3_prepare_v3(c->b
    ,"WITH \"T\"(\"c\")AS(SELECT COUNT(*) FROM \"B\")SELECT \"T\".\"c\">0,\"T\".\"c\"<\"H\".\"l\" FROM \"H\",\"T\" WHERE \"H\".\"i\"=1"
    ,-1, SQLITE_PREPARE_PERSISTENT, &c->sel, 0)
  )
    goto err;
  if (sqlite3_step(c->sel) != SQLITE_ROW)
    goto err;
  s = 0;
  if (sqlite3_column_int(c->sel, 0)) /* not empty */
    s |= chanSsCanGet;
  if (sqlite3_column_int(c->sel, 1)) /* not full */
    s |= chanSsCanPut;
  sqlite3_reset(c->sel);
  sqlite3_step(c->cmt), sqlite3_reset(c->cmt);
  *d = chanStrBlbSQLd;
  *i = chanStrBlbSQLi;
  *v = c;
  return (s);
err:
fprintf(stderr, "SQLite error %s\n", sqlite3_errmsg(c->b));
  sqlite3_step(c->rlb), sqlite3_reset(c->rlb);
  chanStrBlbSQLd(c, 0);
  return (0);
  (void)w; /* not shared yet */
  (void)x; /* not shared yet */
}
