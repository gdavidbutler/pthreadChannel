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

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "sqlite3.h"
#include "chanStrBlbSQL.h"

struct chanStrBlbSQLc {
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

chanSs_t
chanStrBlbSQLa(
  chanStrBlbSQLc_t **c
 ,const char *p
 ,unsigned int o
 ,unsigned int y
 ,sqlite3_int64 z
){
  static const char *jnl[] = {
    "PRAGMA journal_mode=DELETE;"
   ,"PRAGMA journal_mode=TRUNCATE;"
   ,"PRAGMA journal_mode=PERSIST;"
  };
  static const char *syn[] = {
    "PRAGMA synchronous=OFF;"
   ,"PRAGMA synchronous=NORMAL;"
   ,"PRAGMA synchronous=FULL;"
   ,"PRAGMA synchronous=EXTRA;"
  };
  sqlite3_stmt *s;
  int i;

  if (!c)
    return (0);
  if (!p
   || !z
   || o >= sizeof (jnl) / sizeof (jnl[0])
   || y >= sizeof (syn) / sizeof (syn[0])) {
    *c = 0;
    return (0);
  }
  if (!(*c = sqlite3_malloc(sizeof (**c))))
    return (0);
  memset(*c, 0, sizeof (**c));
  if (sqlite3_open_v2(p, &(*c)->d, SQLITE_OPEN_READWRITE, 0)) {
    sqlite3_close((*c)->d);
    if (sqlite3_open_v2(p, &(*c)->d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0))
      goto err;
  }
  if (sqlite3_exec((*c)->d ,"PRAGMA locking_mode=EXCLUSIVE;", 0, 0, 0)
   || sqlite3_exec((*c)->d, jnl[o], 0, 0, 0)
   || sqlite3_exec((*c)->d, syn[y], 0, 0, 0))
    goto err;
  if (sqlite3_exec((*c)->d
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
    "COMMIT;"
   ,0, 0, 0))
    goto err;
  if (sqlite3_prepare_v2((*c)->d
   ,"INSERT OR IGNORE INTO \"H\" VALUES (1,?1,1,1);"
   ,-1, &s, 0))
    goto err;
  sqlite3_bind_int64(s, 1, z); /* largest signed 64bit = 9223372036854775807 */
  if (sqlite3_step(s) != SQLITE_DONE)
    goto err;
  sqlite3_finalize(s);
  if (sqlite3_prepare_v3((*c)->d
   ,"BEGIN IMMEDIATE"
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
/*fprintf(stderr, "SQLite error %s\n", sqlite3_errmsg((*c)->d));*/
  chanStrBlbSQLd((*c), 0);
  return (0);
}

void
chanStrBlbSQLd(
  chanStrBlbSQLc_t *c
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

chanSs_t
chanStrBlbSQLi(
  chanStrBlbSQLc_t *c
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
