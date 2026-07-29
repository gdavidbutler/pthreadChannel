/*
 * Unit test for chanOne array semantics: first-match order, what a timeout
 * reports, and how a shut Channel presents to each operation.
 *
 * All single threaded and deterministic, so it is safe in make check.
 * The concurrent properties (waiter order, the opportunistic completion)
 * are covered by chanAll.pml -DTEST_FAIRNESS, not here.
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
  chan_t *c2;
  chan_t *cs;
  chanArr_t a[3];
  void *v0;
  void *v1;
  void *v2;
  void *p;
  unsigned int i;
  static long One = 1;
  static long Two = 2;

  chanInit(realloc, free);
  if (!(c0 = chanCreate(0, 0)) || !(c1 = chanCreate(0, 0))
   || !(c2 = chanCreate(0, 0)) || !(cs = chanCreate(0, 0))) {
    printf("chanCreate failed\n");
    return (EXIT_FAILURE);
  }

  /* priority is from the base of the array: the lowest ready entry wins */
  p = &One;
  ck(chanOp(0, c1, &p, chanOpPut) == chanOsPut, "seed c1");
  p = &Two;
  ck(chanOp(0, c2, &p, chanOpPut) == chanOsPut, "seed c2");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  a[2].c = c2; a[2].v = &v2; a[2].o = chanOpGet;
  i = chanOne(0, sizeof (a) / sizeof (a[0]), a);
  ck(i == 2, "first ready entry wins, 1 based");
  ck(a[1].s == chanOsGet && v1 == &One, "entry 1 got One");
  ck(a[2].s != chanOsGet, "entry 2 left alone");
  p = 0;
  ck(chanOp(-1, c2, &p, chanOpGet) == chanOsGet && p == &Two, "c2 still holds Two");

  /* an earlier ready entry outranks a later one */
  p = &One;
  ck(chanOp(0, c0, &p, chanOpPut) == chanOsPut, "seed c0");
  p = &Two;
  ck(chanOp(0, c1, &p, chanOpPut) == chanOsPut, "seed c1 again");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  a[2].c = c2; a[2].v = &v2; a[2].o = chanOpGet;
  ck(chanOne(0, sizeof (a) / sizeof (a[0]), a) == 1, "entry 0 outranks entry 1");
  ck(a[0].s == chanOsGet && v0 == &One, "entry 0 got One");
  p = 0;
  ck(chanOp(-1, c1, &p, chanOpGet) == chanOsGet && p == &Two, "c1 undisturbed");

  /* a timeout is not an error: it reports on the first OPERABLE entry */
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  a[2].c = c2; a[2].v = &v2; a[2].o = chanOpGet;
  ck(chanOne(-1, sizeof (a) / sizeof (a[0]), a) == 1, "timeout returns an index, not 0");
  ck(a[0].s == chanOsTmo, "timeout reported on entry 0");

  /* ... skipping entries that are chanOpNop or carry no Channel */
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpNop;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  a[2].c = c2; a[2].v = &v2; a[2].o = chanOpGet;
  ck(chanOne(-1, sizeof (a) / sizeof (a[0]), a) == 2, "timeout skips a chanOpNop entry");
  ck(a[1].s == chanOsTmo, "timeout reported on entry 1");

  memset(a, 0, sizeof (a));
  a[0].c = 0;  a[0].v = &v0; a[0].o = chanOpGet;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpGet;
  a[2].c = c2; a[2].v = &v2; a[2].o = chanOpGet;
  ck(chanOne(-1, sizeof (a) / sizeof (a[0]), a) == 2, "timeout skips a null Channel entry");
  ck(a[1].s == chanOsTmo, "timeout reported on entry 1 again");

  /* a chanOpSht monitor does not fire on a live Channel, so a later
     satisfiable entry still wins */
  p = &One;
  ck(chanOp(0, c2, &p, chanOpPut) == chanOsPut, "seed c2 for monitor test");
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = 0;   a[0].o = chanOpSht;
  a[1].c = c2; a[1].v = &v2; a[1].o = chanOpGet;
  ck(chanOne(0, 2u, a) == 2, "live monitor yields to a ready Get");
  ck(a[1].s == chanOsGet && v2 == &One, "entry 1 got One");

  /* a shut Channel drains first: the Store empties through Get, and only
     then does Get report Sht */
  p = &Two;
  ck(chanOp(0, cs, &p, chanOpPut) == chanOsPut, "seed cs before shut");
  chanShut(cs);
  p = 0;
  ck(chanOp(0, cs, &p, chanOpGet) == chanOsGet && p == &Two, "shut Channel drains");
  ck(chanOp(0, cs, &p, chanOpGet) == chanOsSht, "drained shut Channel reports Sht");

  /* Put onto a shut Channel fails at once, and a monitor fires */
  p = &One;
  ck(chanOp(0, cs, &p, chanOpPut) == chanOsSht, "Put on a shut Channel reports Sht");
  memset(a, 0, sizeof (a));
  a[0].c = cs; a[0].v = 0;   a[0].o = chanOpSht;
  a[1].c = c0; a[1].v = &v0; a[1].o = chanOpGet;
  ck(chanOne(0, 2u, a) == 1, "shut monitor fires");
  ck(a[0].s == chanOsSht, "monitor reports Sht");

  /* nothing to do is an error, matching chanAll */
  memset(a, 0, sizeof (a));
  a[0].c = c0; a[0].v = &v0; a[0].o = chanOpNop;
  a[1].c = c1; a[1].v = &v1; a[1].o = chanOpNop;
  ck(!chanOne(0, 2u, a), "all chanOpNop returns 0");

  chanShut(c0); chanClose(c0);
  chanShut(c1); chanClose(c1);
  chanShut(c2); chanClose(c2);
  chanClose(cs);
  printf("%s\n", Failures ? "FAILURES" : "all pass");
  return (Failures ? EXIT_FAILURE : EXIT_SUCCESS);
}
