/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2022 G. David Butler <gdb@dbSystems.com>
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

struct fw *
fwAlloc(
  unsigned long d
#ifdef FWBLK
,fwNxt_t b /* dependent on target processor cache size */
#endif /* FWBLK */
);

int
fwProcess(
 struct fw *v
#ifdef FWBLK
,fwNxt_t p
#endif /* FWBLK */
);
