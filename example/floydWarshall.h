/* https://en.wikipedia.org/wiki/Floyd–Warshall_algorithm */
/* https://moorejs.github.io/APSP-in-parallel/ */

/* FWEQL keep equal cost next */
/* FWBLK use blocks and threads */

/* pick infinite "cost" char:2^7-1 short:2^15-1 int:2^31-1 long,long:2^63-1 */
typedef short fwCst_t;
/* pick max "vertices"  char:2^8   short:2^16   int:2^32   long,long:2^64 */
typedef unsigned int fwNxt_t;

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
