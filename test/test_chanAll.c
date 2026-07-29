/*
 * Unit test for chanAll all-or-none, and for chanOne/chanAll agreement
 * on a request that asks for nothing.
 *
 * Neither property is reachable from example/: squint.c is the only
 * in-tree chanAll caller and it always passes nsTimeout 0 with live
 * entries, so these paths need a test of their own.
 */

/* Generated with Claude Code (https://claude.ai/code) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chan.h"

static int Failures;

static void
ck(
  int cond
 ,const char *msg
){
  if (!cond) {
    ++Failures;
    printf("FAIL %s\n", msg);
  } else
    printf("ok   %s\n", msg);
}

int
main(
  void
){
  chan_t *c0;
  chan_t *c1;
  chanArr_t a[2];
  void *v0;
  void *v1;
  void *p;
  static long One = 1;
  static long Two = 2;

  chanInit(realloc, free);
  if (!(c0 = chanCreate(0, 0)) || !(c1 = chanCreate(0, 0))) {
    printf("chanCreate failed\n");
    return (EXIT_FAILURE);
  }

  /* only c0 can satisfy a Get: the pair must not partially complete */
  p = &One;
  ck(chanOp(0, c0, &p, chanOpPut) == chanOsPut, "seed c0");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  ck(chanAll(-1, sizeof (a) / sizeof (a[0]), a) == chanAlTmo, "unsatisfiable pair returns chanAlTmo");
  ck(a[0].s != chanOsGet, "slot 0 not reported got");
  ck(a[1].s != chanOsGet, "slot 1 not reported got");
  p = 0;
  ck(chanOp(-1, c0, &p, chanOpGet) == chanOsGet && p == &One, "c0 item NOT consumed");

  /* both satisfiable: the same call must still complete everything */
  p = &One;
  ck(chanOp(0, c0, &p, chanOpPut) == chanOsPut, "seed c0 again");
  p = &Two;
  ck(chanOp(0, c1, &p, chanOpPut) == chanOsPut, "seed c1");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  ck(chanAll(-1, sizeof (a) / sizeof (a[0]), a) == chanAlOp, "satisfiable pair returns chanAlOp");
  ck(a[0].s == chanOsGet && v0 == &One, "slot 0 got One");
  ck(a[1].s == chanOsGet && v1 == &Two, "slot 1 got Two");

  /* mixed: Get ready, Put blocked by a full Store */
  p = &One;
  ck(chanOp(0, c0, &p, chanOpPut) == chanOsPut, "seed c0 for mixed");
  p = &Two;
  ck(chanOp(0, c1, &p, chanOpPut) == chanOsPut, "fill c1 for mixed");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &p;  a[1].o = chanOpPut;
  ck(chanAll(-1, sizeof (a) / sizeof (a[0]), a) == chanAlTmo, "mixed unsatisfiable returns chanAlTmo");
  p = 0;
  ck(chanOp(-1, c0, &p, chanOpGet) == chanOsGet && p == &One, "mixed: c0 item NOT consumed");
  p = 0;
  ck(chanOp(-1, c1, &p, chanOpGet) == chanOsGet && p == &Two, "mixed: c1 unchanged");

  /* nothing to do is a caller error, and both agree on it */
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpNop;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpNop;
  ck(chanAll(0, sizeof (a) / sizeof (a[0]), a) == chanAlErr, "all chanOpNop returns chanAlErr");
  ck(!chanOne(0, sizeof (a) / sizeof (a[0]), a), "all chanOpNop returns chanOne 0");

  memset(a, 0, sizeof (a));
  a[0].c = 0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = 0; a[1].v = &v1; a[1].o = chanOpPut;
  ck(chanAll(0, sizeof (a) / sizeof (a[0]), a) == chanAlErr, "all null Channel returns chanAlErr");
  ck(!chanOne(0, sizeof (a) / sizeof (a[0]), a), "all null Channel returns chanOne 0");

  ck(chanAll(0, 0, a) == chanAlErr, "zero count still chanAlErr");

  /* a live entry alongside a dead one still operates */
  p = &One;
  ck(chanOp(0, c0, &p, chanOpPut) == chanOsPut, "seed c0 for mixed live/dead");
  memset(a, 0, sizeof (a));
  a[0].c = 0;  a[0].v = &v1; a[0].o = chanOpGet;
  a[1].c = c0; a[1].v = &v0; a[1].o = chanOpGet;
  ck(chanAll(0, sizeof (a) / sizeof (a[0]), a) == chanAlOp, "one live entry still operates");
  ck(a[1].s == chanOsGet && v0 == &One, "live entry got One");

  chanShut(c0); chanClose(c0);
  chanShut(c1); chanClose(c1);
  printf("%s\n", Failures ? "FAILURES" : "all pass");
  return (Failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
