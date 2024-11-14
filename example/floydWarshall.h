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

/* https://en.wikipedia.org/wiki/Floyd–Warshall_algorithm */
/* https://moorejs.github.io/APSP-in-parallel/ */

/* FWCST signed type to use for cost */
/* FWNXT unsigned type to use for next */
/* FWEQL keep equal cost next */
/* FWBLK use blocks and threads */

/* pick infinite "cost" char:2^7-1 short:2^15-1 int:2^31-1 long,long:2^63-1 */
#ifdef FWCST
typedef FWCST fwCst_t;
#else
typedef int fwCst_t;
#endif

/* pick max "vertices"  char:2^8   short:2^16   int:2^32   long,long:2^64 */
#ifdef FWNXT
typedef unsigned FWNXT fwNxt_t;
#else
typedef unsigned int fwNxt_t;
#endif

#ifdef FWEQL
struct fwNxt {
  fwNxt_t *x;
  fwNxt_t l; /* if !x, this is the next, otherwise, a count of next */
};
#endif /* FWEQL */
struct fw {
  fwCst_t *cst;      /* cost matrix */
#ifdef FWEQL
  struct fwNxt *nxt; /* next matrix */
#else /* FWEQL */
  fwNxt_t *nxt;      /* next matrix */
#endif /* FWEQL */
  fwNxt_t d;         /* matrix dimension */
#ifdef FWBLK
  fwNxt_t b;         /* blocking factor */
#endif /* FWBLK */
};

void
fwFree(
  struct fw *v
);

/* cost inialized to infinite and next inialized to 0 */
struct fw *
fwAlloc(
  unsigned long d
#ifdef FWBLK
,fwNxt_t b /* dependent on target processor cache size */
#endif /* FWBLK */
);

/* duplicate content */
struct fw *
fwDup(
  const struct fw *v
);

/* compare content */
int
fwCmp(
  const struct fw *v1
 ,const struct fw *v2
);

int
fwProcess(
 struct fw *v
#ifdef FWBLK
,fwNxt_t p
#endif /* FWBLK */
);

#ifdef FWSQLITE

/* SQLITE virtual table read acces to cost array */
/* sqlite3_create_module(sqlite3 *, "fwc", &fwcMod, 0) */
/* sqlite3_prepare_v2(sqlite3 *, "SELECT f AS \"from\", t AS \"to\", c AS \"cost\" FROM Fwc(?1)", -1, sqlite3_stmt *, 0) */
/* sqlite3_bind_pointer(sqlite3_stmt *, 1, struct fw *, "fw", 0) */

/* SQLITE virtual table read acces to next array */
/* sqlite3_create_module(sqlite3 *, "fwn", &fwnMod, 0) */
/* sqlite3_prepare_v2(sqlite3 *, "SELECT f AS \"from\", t AS \"to\", o AS \"ordinal\", n AS \"nextHop\" FROM Fwn(?1)", -1, sqlite3_stmt *, 0) */
/* sqlite3_bind_pointer(sqlite3_stmt *, 1, struct fw *, "fw", 0) */

extern sqlite3_module
fwcMod;
extern sqlite3_module
fwnMod;

#endif /* FWSQLITE */
