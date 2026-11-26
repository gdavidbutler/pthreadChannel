/*
 * Unit test for chanBlbChnRsec
 * Linked against chanBlbTrnFdDatagramStress.c, a replacement transport:
 * an in-process address bus with drop, delay, burst loss, bandwidth,
 * injection and a send observer.
 */

/* Generated with Claude Code (https://claude.ai/code) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "chanBlbTrnFdDatagram.h"
#include "chanBlbChnRsec.h"
#include "rmd128.h"
#include "halfsiphash.h"

/* transport pathology knobs (defined in chanBlbTrnFdDatagramStress.c) */
extern unsigned int chanBlbTrnFdDatagramDelayMs;
extern unsigned int chanBlbTrnFdDatagramDropPct;

/* packet-send observation (defined in chanBlbTrnFdDatagramStress.c) */
#define CHAN_BLB_TRN_FD_DATAGRAM_OBS_MAX 1024
struct chanBlbTrnFdDatagramObsEntry {
  struct timespec ts;
  unsigned int len;
};
extern unsigned int chanBlbTrnFdDatagramObsEnable;
extern unsigned int chanBlbTrnFdDatagramObsCnt;
extern struct chanBlbTrnFdDatagramObsEntry
  chanBlbTrnFdDatagramObs[CHAN_BLB_TRN_FD_DATAGRAM_OBS_MAX];

/* microseconds between two observed packet timestamps (b - a).
 * us, not ms: the inter-shard delay unit is 100 us, so a gap of a
 * few shard-delays is sub-ms; ms granularity would quantize it away. */
static long
obsDeltaUs(
  unsigned int a
 ,unsigned int b
){
  long s;
  long n;

  s = (long)(chanBlbTrnFdDatagramObs[b].ts.tv_sec
           - chanBlbTrnFdDatagramObs[a].ts.tv_sec);
  n = chanBlbTrnFdDatagramObs[b].ts.tv_nsec
    - chanBlbTrnFdDatagramObs[a].ts.tv_nsec;
  return (s * 1000000 + n / 1000);
}

static int Pass;
static int Fail;

static void
check(
  const char *name
 ,int cond
){
  if (cond) {
    ++Pass;
    printf("  PASS: %s\n", name);
  } else {
    ++Fail;
    printf("  FAIL: %s\n", name);
  }
}

/*
 * Datagram-layer hash callbacks -- HalfSipHash-2-4, 64-bit key, 32-bit tag.
 *
 * A cheap keyed PRF as the chanBlbChnRsec datagram GATE: a flood without
 * the key is turned away at one verify.  The 64-bit key is the security
 * ceiling; a real application would derive it from a pre-shared key.
 *
 * adr points to [addrlen(1)][addr(addrlen)] of the datagram.  On egress
 * that's the destination address; on ingress it's the kernel-supplied
 * source address (attacker-controlled -- these test callbacks ignore it).
 */
struct hashKeyCtx {
  unsigned char key[HALFSIPHASH_KEY_SZ];
};

static void
hashSignCb(
  void *ctx
 ,const unsigned char *adr
 ,unsigned char *dst
 ,const unsigned char *src
 ,unsigned int len
){
  struct hashKeyCtx *k;

  (void)adr;
  k = (struct hashKeyCtx *)ctx;
  halfsiphash(src, len, k->key, dst);
}

static int
hashVrfyCb(
  void *ctx
 ,const unsigned char *adr
 ,const unsigned char *mac
 ,const unsigned char *src
 ,unsigned int len
){
  struct hashKeyCtx *k;
  unsigned char computed[HALFSIPHASH_HSH_SZ];
  unsigned char diff;
  unsigned int i;

  (void)adr;
  k = (struct hashKeyCtx *)ctx;
  halfsiphash(src, len, k->key, computed);
  diff = 0;
  for (i = 0; i < HALFSIPHASH_HSH_SZ; ++i)
    diff |= mac[i] ^ computed[i];
  return (diff == 0);
}

/*
 * An RMD128-HMAC as the GATE: a strong MAC a datagram, a holder of the
 * key framed these bytes.  adr is not read.
 */
struct hmacKeyCtx {
  const unsigned char *key;
  unsigned int keyLen;
};

static void
hmacGateSignCb(
  void *ctx
 ,const unsigned char *adr
 ,unsigned char *dst
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacKeyCtx *k;

  (void)adr;
  k = ctx;
  rmd128hmac(k->key, k->keyLen, src, len, dst);
}

static int
hmacGateVrfyCb(
  void *ctx
 ,const unsigned char *adr
 ,const unsigned char *mac
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacKeyCtx *k;
  unsigned char computed[RMD128_SZ];
  unsigned char diff;
  unsigned int i;

  (void)adr;
  k = ctx;
  rmd128hmac(k->key, k->keyLen, src, len, computed);
  for (diff = 0, i = 0; i < sizeof (computed) / sizeof (computed[0]); ++i)
    diff |= mac[i] ^ computed[i];
  return (!diff);
}

/*
 * A GATE keyed by the sender: the halfsiphash key, its first byte XORed
 * with the first fragment's tag byte 0 (src[0]), the byte tagOwner = 1
 * names.  adr is not read.
 */
static void
ownerSignCb(
  void *ctx
 ,const unsigned char *adr
 ,unsigned char *dst
 ,const unsigned char *src
 ,unsigned int len
){
  struct hashKeyCtx *k;
  unsigned char key[HALFSIPHASH_KEY_SZ];

  (void)adr;
  k = ctx;
  memcpy(key, k->key, sizeof (key));
  key[0] ^= src[0];
  halfsiphash(src, len, key, dst);
}

static int
ownerVrfyCb(
  void *ctx
 ,const unsigned char *adr
 ,const unsigned char *mac
 ,const unsigned char *src
 ,unsigned int len
){
  struct hashKeyCtx *k;
  unsigned char key[HALFSIPHASH_KEY_SZ];
  unsigned char computed[HALFSIPHASH_HSH_SZ];
  unsigned char diff;
  unsigned int i;

  (void)adr;
  k = ctx;
  memcpy(key, k->key, sizeof (key));
  key[0] ^= src[0];
  halfsiphash(src, len, key, computed);
  for (diff = 0, i = 0; i < sizeof (computed) / sizeof (computed[0]); ++i)
    diff |= mac[i] ^ computed[i];
  return (!diff);
}

/*
 * Build an RSEC egress blob:
 * [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_us(4)][payload]
 */
static chanBlb_t *
mkBlob(
  const struct sockaddr_in *sa
 ,unsigned int tagSize
 ,unsigned short tag
 ,unsigned char mVal
 ,unsigned long delayUs
 ,const void *payload
 ,unsigned int payloadLen
){
  unsigned char al;
  unsigned int bl;
  unsigned int off;
  unsigned int i;
  chanBlb_t *m;

  al = (unsigned char)sizeof (*sa);
  bl = 1 + al + tagSize + 5 + payloadLen;
  m = (chanBlb_t *)malloc(chanBlb_tSize(bl));
  if (!m)
    return (0);
  m->l = bl;
  m->b[0] = al;
  memcpy(m->b + 1, sa, al);
  off = 1 + al;
  for (i = 0; i < tagSize && i < 2; ++i)
    m->b[off + i] = (unsigned char)((tag >> (i * 8)) & 0xff);
  for (; i < tagSize; ++i)
    m->b[off + i] = 0;
  off += tagSize;
  m->b[off] = mVal;
  m->b[off + 1] = (unsigned char)((delayUs >> 24) & 0xff);
  m->b[off + 2] = (unsigned char)((delayUs >> 16) & 0xff);
  m->b[off + 3] = (unsigned char)((delayUs >> 8) & 0xff);
  m->b[off + 4] = (unsigned char)(delayUs & 0xff);
  if (payloadLen > 0)
    memcpy(m->b + off + 5, payload, payloadLen);
  return (m);
}

/*
 * Verify an RSEC ingress blob:
 * [addrlen(1)][addr(addrlen)][tag(tagSize)][rm(1)][um(1)][payload]
 */
static int
verifyBlob(
  const chanBlb_t *m
 ,unsigned int tagSize
 ,const void *expectedPayload
 ,unsigned int expectedLen
){
  unsigned int al;
  unsigned int skip;

  al = m->b[0];
  skip = 1 + al + tagSize + 2;
  if (m->l < skip)
    return (0);
  if (m->l - skip != expectedLen)
    return (0);
  if (expectedLen > 0 && memcmp(m->b + skip, expectedPayload, expectedLen) != 0)
    return (0);
  return (1);
}

struct testEnv {
  int fd;
  chan_t *outChan;
  chan_t *inChan;
  struct sockaddr_in sa;
};

/*
 * Test-only wrapper that holds the test's intended config in
 * single-struct form and the split egr/igr contexts the library now
 * requires.  setup() copies config fields into both halves; rsecSnap()
 * pulls counters out of both halves.  Test code keeps the original
 * single-rsecCtx style for brevity.
 */
struct rsecPair {
  /* config -- set by the test before setup() */
  unsigned int tagSize;
  unsigned int dgramMax;
  unsigned int tableSize;
  unsigned int msgMax;     /* ingress only */
  unsigned int tagOwner;   /* ingress only */
  /* datagram GATE (dtgHash) */
  unsigned int dtgHashSize;
  void *egrHashCtx; /* each direction its own: no callback */
  void *igrHashCtx; /* context crosses threads */
  void (*dtgHashSign)(void *, const unsigned char *, unsigned char *, const unsigned char *, unsigned int);
  int  (*dtgHashVrfy)(void *, const unsigned char *, const unsigned char *, const unsigned char *, unsigned int);
  /* counter snapshot -- populated by rsecSnap() */
  unsigned int egrMsg, egrFrg, egrFull, egrAgain;
  unsigned int igrFrg, igrHash, igrShape, igrDup, igrLate, igrMsg, igrDcd, igrEvict, igrEvictFull;
  /* private -- populated by setup() */
  struct chanBlbChnRsecEgrCtx egr;
  struct chanBlbChnRsecIgrCtx igr;
};

static void
rsecSnap(
  struct rsecPair *p
){
  struct chanBlbChnRsecEgrCtrs ec;
  struct chanBlbChnRsecIgrCtrs ic;

  chanBlbChnRsecEgrSnap(&p->egr, &ec);
  chanBlbChnRsecIgrSnap(&p->igr, &ic);
  p->egrMsg = ec.msg;
  p->egrFrg = ec.frg;
  p->egrFull = ec.full;
  p->egrAgain = ec.again;
  p->igrFrg = ic.frg;
  p->igrHash = ic.hash;
  p->igrShape = ic.shape;
  p->igrDup = ic.dup;
  p->igrLate = ic.late;
  p->igrMsg = ic.msg;
  p->igrDcd = ic.dcd;
  p->igrEvict = ic.evict;
  p->igrEvictFull = ic.evictFull;
}

static int
setup(
  struct testEnv *env
 ,struct rsecPair *rsecCtx
){
  void *dgramCtx;
  socklen_t sl;
  int one;
  int bufsz;

  /* copy shared config into split contexts (opaque stays zero -- the
   * framer threads allocate / free private state on entry / exit) */
  rsecCtx->egr.dgramMax     = rsecCtx->igr.dgramMax     = rsecCtx->dgramMax;
  rsecCtx->egr.tagSize      = rsecCtx->igr.tagSize      = rsecCtx->tagSize;
  rsecCtx->egr.tableSize    = rsecCtx->igr.tableSize    = rsecCtx->tableSize;
  rsecCtx->igr.msgMax       = rsecCtx->msgMax;
  rsecCtx->igr.tagOwner     = rsecCtx->tagOwner;
  rsecCtx->egr.dtgHashSize  = rsecCtx->igr.dtgHashSize  = rsecCtx->dtgHashSize;
  rsecCtx->egr.dtgHashCtx   = rsecCtx->egrHashCtx;
  rsecCtx->igr.dtgHashCtx   = rsecCtx->igrHashCtx;
  rsecCtx->egr.dtgHashSign  = rsecCtx->dtgHashSign;
  rsecCtx->igr.dtgHashVrfy  = rsecCtx->dtgHashVrfy;
  rsecCtx->egr.opaque       = 0;
  rsecCtx->igr.opaque       = 0;

  memset(&env->sa, 0, sizeof (env->sa));
  env->sa.sin_family = AF_INET;
  env->sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  env->sa.sin_port = 0;
  env->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (env->fd < 0)
    return (0);
  one = 1;
  setsockopt(env->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
  bufsz = 256 * 1024;
  setsockopt(env->fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof (bufsz));
  setsockopt(env->fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof (bufsz));
  if (bind(env->fd, (struct sockaddr *)&env->sa, sizeof (env->sa))) {
    close(env->fd);
    return (0);
  }
  sl = sizeof (env->sa);
  getsockname(env->fd, (struct sockaddr *)&env->sa, &sl);

  dgramCtx = chanBlbTrnFdDatagramCtx(realloc, free);
  if (!dgramCtx) {
    close(env->fd);
    return (0);
  }

  env->outChan = chanCreate(free, 0);
  env->inChan = chanCreate(free, 0);
  if (!env->outChan || !env->inChan) {
    close(env->fd);
    chanBlbTrnFdDatagramFinalClose(dgramCtx);
    return (0);
  }

  if (!chanBlb(realloc, free
      ,env->outChan, chanBlbTrnFdDatagramOutputCtx(dgramCtx, &env->fd, 0, 1, 0), chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, &rsecCtx->egr, chanBlbChnRsecEgr
      ,env->inChan, chanBlbTrnFdDatagramInputCtx(dgramCtx, &env->fd, 0, 1, 0), chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, &rsecCtx->igr, chanBlbChnRsecIgr, 0
      ,dgramCtx, chanBlbTrnFdDatagramFinalClose
      ,0)) {
    close(env->fd);
    return (0);
  }
  return (1);
}

static void
teardown(
  struct testEnv *env
 ,struct rsecPair *rsecCtx
){
  (void)rsecCtx;
  chanShut(env->outChan);
  chanShut(env->inChan);
  shutdown(env->fd, SHUT_RDWR);
  usleep(200000);
  chanClose(env->outChan);
  chanClose(env->inChan);
  usleep(100000);
}

static int
sendBlob(
  chan_t *outChan
 ,chanBlb_t *m
){
  chanArr_t p[1];

  p[0].c = outChan;
  p[0].v = (void **)&m;
  p[0].o = chanOpPut;
  if (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut)
    return (1);
  free(m);
  return (0);
}

static chanBlb_t *
recvBlob(
  chan_t *inChan
 ,long timeoutNs
){
  chanBlb_t *m;
  chanArr_t p[1];

  m = 0;
  p[0].c = inChan;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  if (chanOne(timeoutNs, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet)
    return (m);
  return (0);
}

/* ===== BASIC TESTS (no pathology) ===== */

static void
testBasicM1(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "hello world";

  printf("=== Test: basic m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testBasicM0(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "no parity";

  printf("=== Test: m=0 no parity ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 0, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testDedupM2(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "dedup test";

  printf("=== Test: m=2 de-duplication ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 2, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received once", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  m = recvBlob(env.inChan, 500000000L);
  check("no duplicate", m == 0);
  if (m) free(m);
  teardown(&env, &rsecCtx);
}

/* Completed-entry dedup horizon, DERIVED from the RSEC parameters: a
 * completed entry stops deduping after (k + m) * tableSize further
 * fragment arrivals -- a whole-message re-send under the SAME tag (the
 * BPR retry shape) is then reassembled fresh and DELIVERED AGAIN.
 * Within the horizon it is still eaten (testDedupM2 covers that).
 * The target message is k=1 m=0 at tableSize 8 => horizon = 8
 * arrivals; the flood uses multi-shard messages (k derived from
 * chanBlbChnRsecShard, per the size-derivation discipline) so the
 * arrival count crosses the horizon while table occupancy stays below
 * tableSize -- proving EXPIRY, not LRU eviction, is what redelivers. */
static void
testDedupExpiry(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  char *big;
  const char *msg = "retry me";
  unsigned int shard;
  unsigned int i;

  printf("=== Test: dedup expiry redelivers a re-send ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* (1) k=1 m=0 -- the common small act, and the IDLE RETRY case.  Its
   * train is one fragment, so no straggler is possible and every re-send
   * must be delivered again IMMEDIATELY, with no ambient traffic needed to
   * unblock it.  (An ambient-arrival horizon ate exactly this: at idle the
   * re-sends were both the thing eaten and the only thing advancing the
   * clock.) */
  m = mkBlob(&env.sa, 2, 1, 0, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received once", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, 1, 0, 0, msg, strlen(msg));
    check("idle re-send", sendBlob(env.outChan, m));
    m = recvBlob(env.inChan, 2000000000L);
    check("k=1: every re-send DELIVERED (no ambient traffic)", m != 0);
    if (m) { check("re-send payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  }
  rsecSnap(&rsecCtx);
  check("k=1: nothing eaten as late", rsecCtx.igrLate == 0);

  /* (2) k=3 m=2 -- a 5-fragment train.  The entry completes on the 3rd
   * data shard and absorbs the 2 parity stragglers (counted late); the
   * train is then spent, so the NEXT same-tag fragment is a re-send and
   * reassembles afresh. */
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  check("shard size", shard > 0);
  if (!(big = malloc(3u * shard))) { check("flood alloc", 0); teardown(&env, &rsecCtx); return; }
  memset(big, 'x', 3u * shard);

  m = mkBlob(&env.sa, 2, 50, 2, 0, big, 3u * shard);
  check("multi-shard send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("multi-shard received", m != 0);
  if (m) free(m);

  m = mkBlob(&env.sa, 2, 50, 2, 0, big, 3u * shard);
  check("multi-shard re-send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("train spent: re-send DELIVERED AGAIN", m != 0);
  if (m) free(m);
  free(big);

  rsecSnap(&rsecCtx);
  check("multi-shard parity stragglers were eaten", rsecCtx.igrLate > 0);
  teardown(&env, &rsecCtx);
}

static void
testMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard message requiring several fragments";

  printf("=== Test: multi-shard (dgramMax=16, shardSize=8) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testEmptyPayload(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;

  printf("=== Test: empty payload (leave message) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, 0, 0);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("empty payload", verifyBlob(m, 2, 0, 0)); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testSequential(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  const char *msgs[5];

  msgs[0] = "msg one";
  msgs[1] = "msg two";
  msgs[2] = "msg three";
  msgs[3] = "msg four";
  msgs[4] = "msg five";

  printf("=== Test: 5 sequential messages ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  }
  ok = 0;
  for (i = 0; i < 5; ++i) {
    m = recvBlob(env.inChan, 2000000000L);
    if (m && verifyBlob(m, 2, msgs[i], strlen(msgs[i])))
      ++ok;
    free(m);
  }
  check("all 5 messages correct", ok == 5);
  teardown(&env, &rsecCtx);
}

static void
testTinyShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "stress test with tiny shards yep";

  printf("=== Test: tiny dgramMax=12 shardSize=4 m=2 (many fragments) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 12;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 2, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testVlq2Byte(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "x";

  printf("=== Test: small dgram, 1-byte payload (dgramMax=266) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 266; /* overhead=8, shardSize=258; padding is a single byte */
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, 1);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, 1)); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testVlqMaxShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "x";

  printf("=== Test: VLQ max shard (dgramMax=16392, shardSize=16384) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16392; /* overhead=8, shardSize=16384 (max 2-byte shard_len VLQ) */
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 1-byte payload, shardSize=16384 (exercises the 2-byte shard_len VLQ) */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, 1);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, 1)); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testDgramMaxValidation(void)
{
  printf("=== Test: dgramMax validation (VLQ limit) ===\n");
  /* overhead = tagSize + dtgHashSize + 6: 8 at tag 2, so the 16384
   * shard is reached at 16392 */
  check("16392 shard == 16384", chanBlbChnRsecShard(16392, 2, 0) == 16384);
  check("16392 max > 0", chanBlbChnRsecMax(16392, 2, 0, 1) > 0);
  check("16393 shard == 0", chanBlbChnRsecShard(16393, 2, 0) == 0);
  check("16393 max == 0", chanBlbChnRsecMax(16393, 2, 0, 1) == 0);
  /* a 16-byte gate: overhead 24 */
  check("gate 16408 shard == 16384", chanBlbChnRsecShard(16408, 2, 16) == 16384);
  check("gate 16409 shard == 0", chanBlbChnRsecShard(16409, 2, 16) == 0);
}

static void
testHighParity(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "high parity m=5";

  printf("=== Test: high parity m=5 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 5, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  m = recvBlob(env.inChan, 500000000L);
  check("no duplicate", m == 0);
  if (m) free(m);
  teardown(&env, &rsecCtx);
}

/* ===== MAX PAYLOAD TESTS ===== */

static void
testMaxApi(void)
{
  printf("=== Test: chanBlbChnRsecMax API ===\n");
  /* 520 at tag 2: overhead 8, shard 512 */
  check("m=0 => 256*512=131072", chanBlbChnRsecMax(520, 2, 0, 0) == 131072);
  check("m=1 => 255*512=130560", chanBlbChnRsecMax(520, 2, 0, 1) == 130560);
  check("m=5 => 251*512=128512", chanBlbChnRsecMax(520, 2, 0, 5) == 128512);
  check("m=255 => 1*512=512", chanBlbChnRsecMax(520, 2, 0, 255) == 512);
  check("d=16 m=2 => 254*8=2032", chanBlbChnRsecMax(16, 2, 0, 2) == 2032);
  /* a 16-byte gate: overhead 24, shard 496 */
  check("gate m=0 => 256*496=126976", chanBlbChnRsecMax(520, 2, 16, 0) == 126976);
  check("dgramMax==overhead => 0", chanBlbChnRsecMax(8, 2, 0, 0) == 0);
}

static void
testMaxBoundary(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int maxLen;
  unsigned char *payload;

  printf("=== Test: max payload boundary (dgramMax=16 m=1) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  maxLen = chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize, 1); /* 255 * 8 = 2040 */
  check("maxLen == 2040", maxLen == 2040);

  payload = (unsigned char *)malloc(maxLen + 1);
  if (!payload) { check("malloc", 0); return; }
  memset(payload, 'A', maxLen + 1);

  if (!setup(&env, &rsecCtx)) { check("setup", 0); free(payload); return; }

  /* exactly at the limit: should succeed */
  m = mkBlob(&env.sa, 2, 1, 1, 0, payload, maxLen);
  check("send max", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received max", m != 0);
  if (m) {
    check("max payload correct", verifyBlob(m, 2, payload, maxLen));
    free(m);
  }

  /* one byte over: m clamped from 1 to 0 (k=256, km would be 257) */
  m = mkBlob(&env.sa, 2, 2, 1, 0, payload, maxLen + 1);
  check("send over", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("over-limit received (m clamped)", m != 0);
  if (m) {
    check("over-limit payload correct", verifyBlob(m, 2, payload, maxLen + 1));
    free(m);
  }

  free(payload);
  teardown(&env, &rsecCtx);
}

/* ===== DROP TESTS ===== */

static void
testDropRecovery(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop recovery test message";

  printf("=== Test: 30%% drop, m=7 single shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1, m=7 => 8 shards. Need any 1 of 8. P(all 8 dropped) = 0.3^8 < 0.007% */
  m = mkBlob(&env.sa, 2, 1, 7, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testDropMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop multi shard recovery test!";

  printf("=== Test: 20%% drop, multi-shard dgramMax=18 m=8 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* small dgram => small shards, so the msg spans several */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* 4 data shards, m=8 => 12 total. Need any 4 of 12. */
  m = mkBlob(&env.sa, 2, 1, 8, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testDropMultipleMessages(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  const char *msgs[10];

  msgs[0] = "drop msg 0";
  msgs[1] = "drop msg 1";
  msgs[2] = "drop msg 2";
  msgs[3] = "drop msg 3";
  msgs[4] = "drop msg 4";
  msgs[5] = "drop msg 5";
  msgs[6] = "drop msg 6";
  msgs[7] = "drop msg 7";
  msgs[8] = "drop msg 8";
  msgs[9] = "drop msg 9";

  printf("=== Test: 25%% drop, 10 sequential messages, m=8 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 25;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1, m=8 => 9 shards each. P(all 9 dropped) = 0.25^9 < 0.0004% per msg */
  for (i = 0; i < 10; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 8, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto dropMsgDone; }
  }
  ok = 0;
  for (i = 0; i < 10; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (m && verifyBlob(m, 2, msgs[i], strlen(msgs[i])))
      ++ok;
    free(m);
  }
  check("all 10 messages delivered despite drops", ok == 10);

dropMsgDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

/* ===== DELAY (REORDER) TESTS ===== */

static void
testDelayReorder(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "delayed and reordered fragments";

  printf("=== Test: 50ms delay (reorder), multi-shard dgramMax=16 m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDelayMs = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDelayMs = 0; return; }

  /* 31 bytes / 6 = 6 data + 1 parity = 7 fragments, randomly delayed */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after delay", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDelayMs = 0;
  teardown(&env, &rsecCtx);
}

static void
testDelaySequential(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *msgs[5];

  msgs[0] = "delay seq one";
  msgs[1] = "delay seq two";
  msgs[2] = "delay seq three";
  msgs[3] = "delay seq four";
  msgs[4] = "delay seq five";

  printf("=== Test: 30ms delay, 5 sequential messages ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDelayMs = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDelayMs = 0; return; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto delaySeqDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    for (j = 0; j < 5; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 5 messages delivered despite delay (any order)", ok == 5);

delaySeqDone:
  chanBlbTrnFdDatagramDelayMs = 0;
  teardown(&env, &rsecCtx);
}

/* ===== COMBINED STRESS TESTS ===== */

static void
testStressCombined(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[10];
  const char *msgs[10];

  msgs[0] = "stress msg 0";
  msgs[1] = "stress msg 1";
  msgs[2] = "stress msg 2";
  msgs[3] = "stress msg 3";
  msgs[4] = "stress msg 4";
  msgs[5] = "stress msg 5";
  msgs[6] = "stress msg 6";
  msgs[7] = "stress msg 7";
  msgs[8] = "stress msg 8";
  msgs[9] = "stress msg 9";

  printf("=== Test: 20%% drop + 40ms delay, 10 messages, m=5 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;
  chanBlbTrnFdDatagramDelayMs = 40;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); goto stressDone; }

  for (i = 0; i < 10; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 5, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto stressTeardown; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 10; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    for (j = 0; j < 10; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 10 messages survived drop+delay (any order)", ok == 10);

stressTeardown:
  teardown(&env, &rsecCtx);
stressDone:
  chanBlbTrnFdDatagramDropPct = 0;
  chanBlbTrnFdDatagramDelayMs = 0;
}

static void
testStressMultiShardGate(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hashKeyCtx hk;
  struct hashKeyCtx hkI;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *msgs[5];

  msgs[0] = "gate stress first message here";
  msgs[1] = "gate stress second message go";
  msgs[2] = "gate stress third message ok";
  msgs[3] = "gate stress fourth one too!";
  msgs[4] = "gate stress fifth final msg";

  printf("=== Test: 15%% drop + 30ms delay, multi-shard dgramMax=30 m=6, GATE ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 30; /* overhead=2+4+6=12, shardSize=18 */
  rsecCtx.tableSize = 64;
  for (i = 0; i < sizeof (hk.key); ++i)
    hk.key[i] = 0x69 ^ i;
  rsecCtx.dtgHashSize = HALFSIPHASH_HSH_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashSign = hashSignCb;
  rsecCtx.dtgHashVrfy = hashVrfyCb;

  chanBlbTrnFdDatagramDropPct = 15;
  chanBlbTrnFdDatagramDelayMs = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); goto stressGateDone; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, i + 1, 6, 0, msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto stressGateTeardown; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    unsigned int j;

    if (!(m = recvBlob(env.inChan, 5000000000L)))
      continue;
    for (j = 0; j < 5; ++j)
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    free(m);
  }
  check("all 5 gated multi-shard messages survived stress (any order)", ok == 5);
  rsecSnap(&rsecCtx);
  check("no gate failures", rsecCtx.igrHash == 0);

stressGateTeardown:
  teardown(&env, &rsecCtx);
stressGateDone:
  chanBlbTrnFdDatagramDropPct = 0;
  chanBlbTrnFdDatagramDelayMs = 0;
}

/* ===== COUNTER TESTS ===== */

static void
testCountersBasic(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "counter test";

  printf("=== Test: counters basic m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* k=1 m=1 => 2 fragments sent, 1 message */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 2", rsecCtx.egrFrg == 2);
  check("egrFull == 0", rsecCtx.egrFull == 0);
  check("igrFrg == 2", rsecCtx.igrFrg == 2);
  check("igrHash == 0", rsecCtx.igrHash == 0);
  check("igrShape == 0", rsecCtx.igrShape == 0);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDcd == 0", rsecCtx.igrDcd == 0);
  check("igrDup == 0", rsecCtx.igrDup == 0);
  check("igrLate > 0", rsecCtx.igrLate > 0);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  teardown(&env, &rsecCtx);
}

static void
testCountersEgrFull(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  const char *msg = "egrFull trigger";

  /* With tableSize=1 and slot dwell (k+m)*delay ~ 25 ms, three
   * back-to-back sends must enroll serially: msg 1 enrolls
   * immediately, msgs 2 and 3 each find the slot full when egress
   * picks them up.  Expected egrFull = 2. */
  printf("=== Test: egrFull counter (tableSize=1, serialized) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 1;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 3; ++i) {
    /* mVal=4 -> k=1+m=5 shards; delay=5000us -> ~25 ms slot dwell */
    m = mkBlob(&env.sa, 2, (unsigned short)(0x100 + i), 4, 5000,
               msg, strlen(msg));
    check("send", sendBlob(env.outChan, m));
  }
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    check("received", m != 0);
    if (m) free(m);
  }
  /* egrMsg increments only after ALL shards of a message have been
   * emitted (recvBlob returns as soon as k shards decode).  Wait long
   * enough for the egress thread to finish draining the last
   * message's shards (~250 ms per message dwell). */
  usleep(800000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 3", rsecCtx.egrMsg == 3);
  check("egrFull == 2", rsecCtx.egrFull == 2);
  teardown(&env, &rsecCtx);
}

static void
testCountersMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard counter test msg!";
  unsigned int shard;
  unsigned int k;

  printf("=== Test: counters multi-shard m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* k data shards (derived from the API shard size) + m=1 parity */
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  k = (strlen(msg) + shard - 1) / shard;
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == k+1", rsecCtx.egrFrg == k + 1);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDcd == 0", rsecCtx.igrDcd == 0);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  teardown(&env, &rsecCtx);
}

static void
testCountersDrop(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop counter test message!!";
  unsigned int shard;
  unsigned int k;

  printf("=== Test: counters with 20%% drop, multi-shard m=8 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* small dgram => small shards, so the msg spans several */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k data shards (derived from the API shard size) + m=8 parity */
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  k = (strlen(msg) + shard - 1) / shard;
  m = mkBlob(&env.sa, 2, 1, 8, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == k+8", rsecCtx.egrFrg == k + 8);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  /* if any data shard was dropped, RS decode was needed */
  printf("    (igrDcd=%u igrFrg=%u igrDup=%u)\n",
    rsecCtx.igrDcd, rsecCtx.igrFrg, rsecCtx.igrDup);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testCountersDedup(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "dedup counter";

  printf("=== Test: counters de-dup (m=2 => 3 frags, 2 late) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* k=1 m=2 => 3 fragments. First arrival delivers. 0 are dup. */
  m = mkBlob(&env.sa, 2, 1, 2, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) free(m);
  m = recvBlob(env.inChan, 500000000L);
  check("no duplicate delivery", m == 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 3", rsecCtx.egrFrg == 3);
  check("igrFrg == 3", rsecCtx.igrFrg == 3);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDup == 0", rsecCtx.igrDup == 0);
  check("igrLate == 2", rsecCtx.igrLate == 2);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  teardown(&env, &rsecCtx);
}

/* ===== SHARD SIZE API TESTS ===== */

static void
testShardApi(void)
{
  printf("=== Test: chanBlbChnRsecShard API ===\n");
  check("shard=512", chanBlbChnRsecShard(520, 2, 0) == 512);
  check("shard=508 with a 4-byte gate", chanBlbChnRsecShard(520, 2, 4) == 508);
  check("shard=496 with a 16-byte gate", chanBlbChnRsecShard(520, 2, 16) == 496);
  check("shard=494 with tag 4 and a 16-byte gate", chanBlbChnRsecShard(520, 4, 16) == 494);
  check("shard=8 small dgram", chanBlbChnRsecShard(16, 2, 0) == 8);
  check("dgramMax==overhead => 0", chanBlbChnRsecShard(8, 2, 0) == 0);
}

/* ===== HDR CALLBACK TESTS ===== */

/* ===== EGRESS PACING TESTS ===== */

static void
testEgressInterleave(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msgs[2];
  int seen[2];
  unsigned int i;
  unsigned int ok;

  msgs[0] = "interleave one";
  msgs[1] = "interleave two";

  printf("=== Test: egress interleave tableSize=2 delay=1000us ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 2;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 1000, msgs[0], strlen(msgs[0]));
  check("send1", sendBlob(env.outChan, m));
  m = mkBlob(&env.sa, 2, 2, 1, 1000, msgs[1], strlen(msgs[1]));
  check("send2", sendBlob(env.outChan, m));

  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 2; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    for (j = 0; j < 2; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("both received", ok == 2);
  usleep(100000);
  rsecSnap(&rsecCtx);
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  teardown(&env, &rsecCtx);
}

static void
testEgressBackpressure(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg1 = "first msg";
  const char *msg2 = "second msg";
  int seen1;
  int seen2;
  unsigned int i;

  printf("=== Test: egress backpressure tableSize=1 delay=5000us ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 1;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob1: k=1 m=1 km=2, delay=5000us => paced over 5ms */
  m = mkBlob(&env.sa, 2, 1, 1, 5000, msg1, strlen(msg1));
  check("send1", sendBlob(env.outChan, m));
  /* blob2 blocks until blob1 completes (backpressure) */
  m = mkBlob(&env.sa, 2, 2, 1, 0, msg2, strlen(msg2));
  check("send2", sendBlob(env.outChan, m));

  /* both messages delivered */
  seen1 = 0;
  seen2 = 0;
  for (i = 0; i < 2; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    if (verifyBlob(m, 2, msg1, strlen(msg1)))
      seen1 = 1;
    else if (verifyBlob(m, 2, msg2, strlen(msg2)))
      seen2 = 1;
    free(m);
  }
  check("blob1 received", seen1);
  check("blob2 received", seen2);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  teardown(&env, &rsecCtx);
}

static void
testCountersBackpressure(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg1 = "backpressure counter test!!";
  const char *msg2 = "ok";
  int seen1;
  int seen2;
  unsigned int i;
  unsigned int shard;
  unsigned int k;

  printf("=== Test: backpressure counter tableSize=1 multi-shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 1;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob1: k data shards (from API) + m=2 parity, delay=1000us; blob2: 2 bytes => 1 frag */
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  k = (strlen(msg1) + shard - 1) / shard;
  m = mkBlob(&env.sa, 2, 1, 2, 1000, msg1, strlen(msg1));
  check("send1", sendBlob(env.outChan, m));
  /* blob2 blocks until blob1 completes (backpressure) */
  m = mkBlob(&env.sa, 2, 2, 0, 0, msg2, strlen(msg2));
  check("send2", sendBlob(env.outChan, m));

  /* both messages delivered */
  seen1 = 0;
  seen2 = 0;
  for (i = 0; i < 2; ++i) {
    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    if (verifyBlob(m, 2, msg1, strlen(msg1)))
      seen1 = 1;
    else if (verifyBlob(m, 2, msg2, strlen(msg2)))
      seen2 = 1;
    free(m);
  }
  check("blob1 received", seen1);
  check("blob2 received", seen2);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  /* blob1: k+2 frags, blob2: 1 frag */
  check("egrFrg == (k+2)+1", rsecCtx.egrFrg == (k + 2) + 1);
  teardown(&env, &rsecCtx);
}

/* ===== COVERAGE GAP TESTS ===== */

static void
testRsDecodeAliasing(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int j;
  unsigned int ok;
  int seen[3];
  const char *msgs[3];

  msgs[0] = "rs decode alias test msg zero";
  msgs[1] = "rs decode alias test msg one!";
  msgs[2] = "rs decode alias test msg two!";

  printf("=== Test: RS decode with mixed data+parity (aliasing fix) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* small dgram => small shards: ~29-byte msgs span a few */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=3 m=20 => 23 shards, need 3. 50% drop => P(loss) tiny per msg */
  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 20, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto rsAliasDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    for (j = 0; j < 3; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 3 messages delivered", ok == 3);
  rsecSnap(&rsecCtx);
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);

rsAliasDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testMClampIntermediate(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char *payload;
  unsigned int payloadLen;
  unsigned int i;
  unsigned int shard;
  unsigned int km;

  printf("=== Test: m clamping k=250 m=10->6 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* small dgram => small shards (size from API below) */
  rsecCtx.tableSize = 64;

  /* force exactly k=250 data shards; m=10 requested => km=260>256 => m clamped */
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  payloadLen = 250 * shard;
  km = 250 + 10;
  if (km > 256)
    km = 256;
  payload = (unsigned char *)malloc(payloadLen);
  if (!payload) { check("malloc", 0); return; }
  for (i = 0; i < payloadLen; ++i)
    payload[i] = (unsigned char)(i & 0xff);

  if (!setup(&env, &rsecCtx)) { check("setup", 0); free(payload); return; }

  m = mkBlob(&env.sa, 2, 1, 10, 0, payload, payloadLen);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 10000000000L);
  check("received", m != 0);
  if (m) {
    check("payload correct", verifyBlob(m, 2, payload, payloadLen));
    free(m);
  }
  usleep(100000);
  rsecSnap(&rsecCtx);
  /* m clamped from 10 to 6: 250 data + 6 parity = 256 fragments */
  check("egrFrg == km (clamped)", rsecCtx.egrFrg == km);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);

  free(payload);
  teardown(&env, &rsecCtx);
}

static void
testMalformedBlob(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;

  printf("=== Test: malformed blob protocol violation ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob too short: only 2 bytes, needs at least prefixSize+egrhdr */
  m = (chanBlb_t *)malloc(chanBlb_tSize(2));
  if (!m) { check("malloc", 0); teardown(&env, &rsecCtx); return; }
  m->l = 2;
  m->b[0] = 1;
  m->b[1] = 0;
  check("send malformed", sendBlob(env.outChan, m));

  /* egress drops the malformed blob and keeps running */
  m = recvBlob(env.inChan, 1000000000L);
  check("no message from malformed blob", m == 0);
  if (m) free(m);

  /* a subsequent valid blob proves the egress survived */
  m = mkBlob(&env.sa, 2, 1, 1, 0, "ok", 2);
  check("send valid after malformed", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("valid blob delivered (egress still running)", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, "ok", 2)); free(m); }

  teardown(&env, &rsecCtx);
}

static void
testEmptyPayloadDrop(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;

  printf("=== Test: empty payload with 30%% drop, m=7 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1 m=7 => 8 shards, need 1. P(all dropped) = 0.3^8 < 0.007% */
  m = mkBlob(&env.sa, 2, 1, 7, 0, 0, 0);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("empty payload", verifyBlob(m, 2, 0, 0)); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testTagSizeZero(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "zero tag size test";

  printf("=== Test: tagSize=0 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 0;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 0, 0, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 0, msg, strlen(msg))); free(m); }

  teardown(&env, &rsecCtx);
}

static void
testLargeK(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char *payload;
  unsigned int payloadLen;
  unsigned int i;
  unsigned int shard;
  unsigned int k;

  printf("=== Test: large k=250 m=1 (251 fragments) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* small dgram => small shards (size from API below) */
  rsecCtx.tableSize = 64;

  /* force exactly k=250 data shards via the API shard size; m=1 => k+1 fragments */
  k = 250;
  shard = chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize);
  payloadLen = k * shard;
  payload = (unsigned char *)malloc(payloadLen);
  if (!payload) { check("malloc", 0); return; }
  for (i = 0; i < payloadLen; ++i)
    payload[i] = (unsigned char)(i & 0xff);

  if (!setup(&env, &rsecCtx)) { check("setup", 0); free(payload); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, payload, payloadLen);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received", m != 0);
  if (m) {
    check("payload correct", verifyBlob(m, 2, payload, payloadLen));
    free(m);
  }
  usleep(100000);
  rsecSnap(&rsecCtx);
  check("egrFrg == k+1", rsecCtx.egrFrg == k + 1);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);

  free(payload);
  teardown(&env, &rsecCtx);
}

static void
testGateHmacRsDecode(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hk;
  struct hmacKeyCtx hkI;
  chanBlb_t *m;
  unsigned int i;
  unsigned int j;
  unsigned int ok;
  int seen[5];
  static const unsigned char key[] = "hmacRsDecode";
  const char *msgs[5];

  msgs[0] = "hmac rs decode msg zero!";
  msgs[1] = "hmac rs decode msg one!!";
  msgs[2] = "hmac rs decode msg two!!";
  msgs[3] = "hmac rs decode msg 3!!!";
  msgs[4] = "hmac rs decode msg 4!!!";

  printf("=== Test: RMD128-HMAC GATE + RS decode (50%% drop dgramMax=40 m=20) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 40; /* overhead=2+16+6=24, shardSize=16: k=2 */
  rsecCtx.tableSize = 64;
  hk.key = key;
  hk.keyLen = sizeof (key) - 1;
  rsecCtx.dtgHashSize = RMD128_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashSign = hmacGateSignCb;
  rsecCtx.dtgHashVrfy = hmacGateVrfyCb;

  chanBlbTrnFdDatagramDropPct = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=2 m=20 km=22, 50% drop, 5 msgs: P(any fail)<0.003%, P(no decode)<0.1% */
  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, i + 1, 20, 0, msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto hmacRsDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    if (!(m = recvBlob(env.inChan, 5000000000L)))
      continue;
    for (j = 0; j < 5; ++j)
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    free(m);
  }
  check("all 5 HMAC-gated messages delivered", ok == 5);
  rsecSnap(&rsecCtx);
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);
  check("no gate failures", rsecCtx.igrHash == 0);

hmacRsDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

/* ===== PACKING TESTS ===== */

static void
testPackSmall(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *msgs[5];

  msgs[0] = "pack small one";
  msgs[1] = "pack small two";
  msgs[2] = "pack small 3!!";
  msgs[3] = "pack small 4!!";
  msgs[4] = "pack small 5!!";

  printf("=== Test: packing multiple small blobs to same address ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    {
      unsigned int j;

      for (j = 0; j < 5; ++j) {
        if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
          seen[j] = 1;
          ++ok;
          break;
        }
      }
    }
    free(m);
  }
  check("all 5 small packed messages delivered", ok == 5);
  teardown(&env, &rsecCtx);
}

static void
testPackMixedSize(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int ok;
  int seen[3];
  unsigned int i;
  const char *small1 = "tiny";
  const char *small2 = "also tiny";
  unsigned char big[400];

  printf("=== Test: packing mixed small and large blobs ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  for (i = 0; i < sizeof (big) / sizeof (big[0]); ++i)
    big[i] = (unsigned char)(i & 0xff);

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* send two small and one large */
  m = mkBlob(&env.sa, 2, 1, 1, 0, small1, strlen(small1));
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  m = mkBlob(&env.sa, 2, 2, 1, 0, small2, strlen(small2));
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  m = mkBlob(&env.sa, 2, 3, 1, 0, big, sizeof (big) / sizeof (big[0]));
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }

  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    if (!seen[0] && verifyBlob(m, 2, small1, strlen(small1))) {
      seen[0] = 1; ++ok;
    } else if (!seen[1] && verifyBlob(m, 2, small2, strlen(small2))) {
      seen[1] = 1; ++ok;
    } else if (!seen[2] && verifyBlob(m, 2, big, sizeof (big) / sizeof (big[0]))) {
      seen[2] = 1; ++ok;
    }
    free(m);
  }
  check("all 3 mixed-size messages delivered", ok == 3);
  teardown(&env, &rsecCtx);
}

static void
testPackDrop(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *msgs[5];

  msgs[0] = "pack drop one";
  msgs[1] = "pack drop two";
  msgs[2] = "pack drop 3!!";
  msgs[3] = "pack drop 4!!";
  msgs[4] = "pack drop 5!!";

  printf("=== Test: packing with 20%% drops m=5 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1 m=5 km=6, 20% drop: P(fail per msg)=0.2^6=0.0006%, P(any of 5)<0.003% */
  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 5, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto packDropDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    {
      unsigned int j;

      for (j = 0; j < 5; ++j) {
        if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
          seen[j] = 1;
          ++ok;
          break;
        }
      }
    }
    free(m);
  }
  check("all 5 packed messages delivered despite drops", ok == 5);

packDropDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

/*
 * Packing under a GATE keyed by the sender, tagOwner = 1: three
 * messages from one sender (tag byte 0 is 1, byte 1 differs), each k=1
 * m=3 paced at 20 ms, so shards of different messages fall due together
 * and share datagrams.  Every one passes the gate and the sender rule.
 */
static void
testPackOwnerGate(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hashKeyCtx hk;
  struct hashKeyCtx hkI;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[3];
  const char *msgs[3];

  msgs[0] = "pack owner msg zero";
  msgs[1] = "pack owner msg one!";
  msgs[2] = "pack owner msg two!";

  printf("=== Test: packing under a gate keyed by the sender (tagOwner) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;
  rsecCtx.tagOwner = 1;
  for (i = 0; i < sizeof (hk.key); ++i)
    hk.key[i] = 0x2d ^ i;
  rsecCtx.dtgHashSize = HALFSIPHASH_HSH_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashSign = ownerSignCb;
  rsecCtx.dtgHashVrfy = ownerVrfyCb;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;
  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 4, (i + 1) << 8 | 1, 3, 20000, msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto packOwnerDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    unsigned int j;

    if (!(m = recvBlob(env.inChan, 3000000000L)))
      continue;
    for (j = 0; j < 3; ++j)
      if (!seen[j] && verifyBlob(m, 4, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    free(m);
  }
  check("all 3 messages delivered", ok == 3);
  usleep(300000);
  rsecSnap(&rsecCtx);
  printf("    (%u fragments in %u datagrams)\n", rsecCtx.egrFrg, chanBlbTrnFdDatagramObsCnt);
  check("12 fragments sent", rsecCtx.egrFrg == 12);
  check("some datagram packed more than one", chanBlbTrnFdDatagramObsCnt < rsecCtx.egrFrg);
  check("no gate or sender failures", rsecCtx.igrHash == 0);
  check("every fragment parsed", rsecCtx.igrFrg == 12);

packOwnerDone:
  chanBlbTrnFdDatagramObsEnable = 0;
  teardown(&env, &rsecCtx);
}

static void
testPackCounters(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  const char *msgs[3];

  msgs[0] = "pack cnt one";
  msgs[1] = "pack cnt two";
  msgs[2] = "pack cnt 3!!";

  printf("=== Test: packing fragment counter correctness ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 3 messages, each k=1 m=1 => 2 fragments each => 6 total fragments */
  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  }
  ok = 0;
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (m) { ++ok; free(m); }
  }
  check("all 3 received", ok == 3);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 3", rsecCtx.egrMsg == 3);
  check("egrFrg == 6", rsecCtx.egrFrg == 6);
  check("igrMsg == 3", rsecCtx.igrMsg == 3);
  teardown(&env, &rsecCtx);
}

/* ===== PACING TESTS =====
 * Verify that egress packet emission honors the inter-shard delay.
 * The delay byte is in 100-microsecond units (see mkBlob), so gaps
 * are measured in us (obsDeltaUs) and inputs are chosen so the
 * expected gap sits well above macOS scheduler jitter (tol below).
 *   - Same-message shards never pack together (one packet each).
 *   - After emitting a packet, the next emit waits the SUM of the
 *     delays of the shards actually packed into that packet.
 * The observation hook in chanBlbTrnFdDatagramStress.c records
 * caller-side send timestamps into chanBlbTrnFdDatagramObs[]. */

static void
testPaceSingleMessage(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "paced single";
  unsigned long delayUs;
  unsigned int i;
  unsigned int ok;
  long tol;

  /* k=1 m=4 => 5 shards, all from one message.  Same-message shards
   * never pack together, so each shard travels in its own datagram
   * and consecutive packets must be >= delayUs apart.
   * delayUs=20000 => 20ms gaps, comfortably above the tolerance. */
  delayUs = 20000;
  tol = 3000; /* us tolerance for clock/scheduling jitter */

  printf("=== Test: pace single message k=1 m=4 delay=20000us (20ms) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;

  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  m = mkBlob(&env.sa, 2, 1, 4, delayUs, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  /* wait for all 5 shards to be emitted (window ~ 5*20ms = 100ms) */
  usleep(300000);
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("egrFrg == 5", rsecCtx.egrFrg == 5);
  check("5 datagrams emitted", chanBlbTrnFdDatagramObsCnt == 5);
  ok = 1;
  for (i = 1; i < chanBlbTrnFdDatagramObsCnt; ++i) {
    long g;

    g = obsDeltaUs(i - 1, i);
    if (g < (long)delayUs - tol) {
      printf("    gap[%u]=%ldus < %ldus\n", i, g, (long)delayUs - tol);
      ok = 0;
    }
  }
  check("all inter-packet gaps >= delay", ok);

  teardown(&env, &rsecCtx);
}

static void
testPacePackingDensity(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *msgs[5];

  msgs[0] = "pack dense a";
  msgs[1] = "pack dense b";
  msgs[2] = "pack dense c";
  msgs[3] = "pack dense d";
  msgs[4] = "pack dense e";

  /* 5 messages to the same address, k=1 m=0 each.  Shards schedule
   * within a narrow window -- packets emitted after the first should
   * coalesce several shards each. ObsCnt < egrFrg proves packing. */
  printf("=== Test: packing reduces datagram count ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;

  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 0, 1000,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) {
      check("send", 0);
      chanBlbTrnFdDatagramObsEnable = 0;
      teardown(&env, &rsecCtx);
      return;
    }
  }

  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    for (j = 0; j < 5; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 5 messages delivered", ok == 5);
  usleep(150000);
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("egrFrg == 5", rsecCtx.egrFrg == 5);
  check("packed: ObsCnt < egrFrg", chanBlbTrnFdDatagramObsCnt < rsecCtx.egrFrg);
  teardown(&env, &rsecCtx);
}

static void
testPaceMaxDelayMixed(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msgA = "slow";
  const char *msgB = "fast";
  unsigned long delayA;
  unsigned long delayB;
  int seenA;
  int seenB;
  unsigned int i;
  unsigned int hasSlowGap;
  unsigned int sawFastGap;

  /* Construct a scenario that exercises the packed-packet delay rule
   * (SUM of packed shards' delays since 2026-07-13; was max).  Delays
   * are in microseconds; a 2:1 ratio keeps the fast delay (12ms) well
   * above jitter:
   *   msg A: k=1 m=1 (2 shards) delay=24000 (24ms) -- scheduled at +24,+48ms
   *   msg B: k=1 m=3 (4 shards) delay=12000 (12ms) -- scheduled at +12,+24,+36,+48ms
   * Both to the same address, queued back-to-back.  Expected emissions
   * (approximate, dropping insertion jitter):
   *   P1 @12ms:  B0                        -> lastPackD=12ms
   *   P2 @24ms:  A0 + B1 (packed, both due) -> lastPackD=24+12=36ms
   *   P3 @60ms (= P2 + 36ms): B2 + A1 (both due, gated) -> lastPackD=36ms
   *   P4 @96ms (= P3 + 36ms): B3                -> lastPackD=12ms
   * The critical check is a gap >= 24ms (delayA): a bug that used min
   * (or ignored the slow shard's share of the packed sum) would wait
   * only 12ms after the A0+B1 pack, so no gap would reach delayA. */
  delayA = 24000;
  delayB = 12000;

  printf("=== Test: pace respects packed-packet delay sum ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;

  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  /* queue A first so its slot is claimed first; both blobs land in
   * the schedule before the first emission at t=~20ms */
  m = mkBlob(&env.sa, 2, 1, 1, delayA, msgA, strlen(msgA));
  check("sendA", sendBlob(env.outChan, m));
  m = mkBlob(&env.sa, 2, 2, 3, delayB, msgB, strlen(msgB));
  check("sendB", sendBlob(env.outChan, m));

  seenA = 0;
  seenB = 0;
  for (i = 0; i < 2; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    if (verifyBlob(m, 2, msgA, strlen(msgA)))
      seenA = 1;
    else if (verifyBlob(m, 2, msgB, strlen(msgB)))
      seenB = 1;
    free(m);
  }
  check("msgA received", seenA);
  check("msgB received", seenB);

  usleep(400000); /* wait for all shards to be emitted (~96ms + margin) */
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("egrFrg == 6", rsecCtx.egrFrg == 6);
  /* 6 shards packed into 4..6 datagrams depending on insertion
   * jitter (how many B shards overlap the A shards at the same tick).
   * Count isn't the invariant; the inter-packet delays are. */
  check("ObsCnt in [4,6]",
        chanBlbTrnFdDatagramObsCnt >= 4
     && chanBlbTrnFdDatagramObsCnt <= 6);

  hasSlowGap = 0;
  sawFastGap = 0;
  for (i = 1; i < chanBlbTrnFdDatagramObsCnt; ++i) {
    long g;

    g = obsDeltaUs(i - 1, i);
    /* every gap must at minimum honor the fast delay */
    if (g >= (long)delayB - 3000)
      ++sawFastGap;
    /* at least one gap must honor the slow delay */
    if (g >= (long)delayA - 3000)
      hasSlowGap = 1;
  }
  check("every gap >= delayB (fast minimum)",
        sawFastGap == chanBlbTrnFdDatagramObsCnt - 1);
  check("some gap >= delayA (slow drives one wait)", hasSlowGap);

  teardown(&env, &rsecCtx);
}

static void
testPaceNoBlasting(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[3];
  const char *msgs[3];
  unsigned long delayUs;

  msgs[0] = "no blast alpha";
  msgs[1] = "no blast bravo";
  msgs[2] = "no blast charlie";

  /* Three k=1 m=2 messages (3 shards each = 9 shards total) paced at
   * delay=20000us (20ms).  Maximal packing finishes in 3
   * datagrams (shards from all 3 messages coalesced per tick); no
   * packing emits 9.  The bound we verify regardless of packing: no
   * inter-packet gap may be smaller than the delay.  If the pacing
   * logic were bypassed, the egress would blast shards back-to-back. */
  delayUs = 20000;

  printf("=== Test: pace 3 messages never blast (delay=20000us, 20ms) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;

  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 2, delayUs,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) {
      check("send", 0);
      chanBlbTrnFdDatagramObsEnable = 0;
      teardown(&env, &rsecCtx);
      return;
    }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    for (j = 0; j < 3; ++j) {
      if (!seen[j] && verifyBlob(m, 2, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 3 messages delivered", ok == 3);
  usleep(400000);
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("egrFrg == 9", rsecCtx.egrFrg == 9);
  /* at least as many packets as shards-per-message (same-msg shards
   * never pack), at most one packet per shard */
  check("ObsCnt in [3,9]",
        chanBlbTrnFdDatagramObsCnt >= 3
     && chanBlbTrnFdDatagramObsCnt <= 9);
  ok = 1;
  for (i = 1; i < chanBlbTrnFdDatagramObsCnt; ++i) {
    long g;

    g = obsDeltaUs(i - 1, i);
    if (g < (long)delayUs - 3000) {
      printf("    gap[%u]=%ldus < %ldus\n", i, g, (long)delayUs - 3000);
      ok = 0;
    }
  }
  check("no gap faster than delay", ok);

  teardown(&env, &rsecCtx);
}

static void
testPackSameMessage(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "same msg shards not packed together";

  printf("=== Test: same-message shards NOT packed together ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* k=1 m=3 => 4 shards, all same message. Should NOT pack together */
  m = mkBlob(&env.sa, 2, 1, 3, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  usleep(100000);
  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  /* 4 fragments from one message: none should be packed together */
  check("egrFrg == 4", rsecCtx.egrFrg == 4);
  teardown(&env, &rsecCtx);
}

/*
 * Same TAG to one address, two messages: the second is held until the
 * first's slot frees -- a second train under the tag would feed the one
 * reassembly at the ingress -- and then goes as a train of its own.  Two
 * k=1 m=3 blobs under one tag, paced at 20 ms: eight datagrams, none
 * carrying two fragments, and each train delivered, the second once the
 * first's k + m fragments have all come.
 */
static void
testPackSameTag(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "same tag, two trains";
  unsigned int single;
  unsigned int i;
  unsigned int ok;

  printf("=== Test: same tag at one address held until its slot frees ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramObsCnt = 0;
  chanBlbTrnFdDatagramObsEnable = 1;

  if (!setup(&env, &rsecCtx)) {
    check("setup", 0);
    chanBlbTrnFdDatagramObsEnable = 0;
    return;
  }

  m = mkBlob(&env.sa, 2, 0x5a5a, 3, 20000, msg, strlen(msg));
  check("send first", sendBlob(env.outChan, m));
  m = mkBlob(&env.sa, 2, 0x5a5a, 3, 20000, msg, strlen(msg));
  check("send second", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("first train delivered", m && verifyBlob(m, 2, msg, strlen(msg)));
  if (m) free(m);
  m = recvBlob(env.inChan, 3000000000L);
  check("second train delivered", m && verifyBlob(m, 2, msg, strlen(msg)));
  if (m) free(m);

  usleep(200000);
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("the second held once (egrAgain == 1)", rsecCtx.egrAgain == 1);
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  check("egrFrg == 8", rsecCtx.egrFrg == 8);
  check("8 datagrams emitted", chanBlbTrnFdDatagramObsCnt == 8);
  if (chanBlbTrnFdDatagramObsCnt != 8) {
    printf("    %u datagrams:", chanBlbTrnFdDatagramObsCnt);
    for (i = 0; i < chanBlbTrnFdDatagramObsCnt; ++i)
      printf(" %u", chanBlbTrnFdDatagramObs[i].len);
    printf("\n");
  }
  /* one fragment per datagram: [tag][k-1][m][si][pad][vlq(1)][shard],
   * the shard the whole payload at k = 1 */
  single = 2 + 5 + strlen(msg);
  for (ok = 1, i = 0; i < chanBlbTrnFdDatagramObsCnt; ++i)
    if (chanBlbTrnFdDatagramObs[i].len != single) {
      printf("    datagram[%u] len %u != %u\n", i, chanBlbTrnFdDatagramObs[i].len, single);
      ok = 0;
    }
  check("no datagram carries two fragments", ok);
  check("igrLate == 6 (each train's three stragglers)", rsecCtx.igrLate == 6);

  teardown(&env, &rsecCtx);
}

/* ===== rm/um + raw-injection coverage ===== */

/*
 * Inject a hand-built datagram into the transport's address bus.
 */
extern int
chanBlbTrnFdDatagramInject(
  const struct sockaddr *dst
 ,socklen_t dstLen
 ,const struct sockaddr *src
 ,socklen_t srcLen
 ,const unsigned char *payload
 ,unsigned int payloadLen
);

/*
 * One fragment at dg: [tag(tagSize)][k-1][m][si][padding][shardLen
 * (1-byte VLQ, < 128)][shard]; its length.
 */
static unsigned int
mkFrag(
  unsigned char *dg
 ,const unsigned char *tagBytes
 ,unsigned int tagSize
 ,unsigned char kMinus1
 ,unsigned char mVal
 ,unsigned char si
 ,unsigned char padding
 ,const unsigned char *shardData
 ,unsigned char shardLen
){
  unsigned int pos;

  memcpy(dg, tagBytes, tagSize);
  pos = tagSize;
  dg[pos++] = kMinus1;
  dg[pos++] = mVal;
  dg[pos++] = si;
  dg[pos++] = padding;
  dg[pos++] = shardLen;
  memcpy(dg + pos, shardData, shardLen);
  return (pos + shardLen);
}

/*
 * Inject the wire bytes dg of one datagram to dst from a synthetic
 * source, 127.0.0.1 port 1: the delivered blob's addr prefix is
 * well-formed and its content irrelevant here.
 */
static int
injectDg(
  const struct sockaddr_in *dst
 ,const unsigned char *dg
 ,unsigned int len
){
  struct sockaddr_in src;

  memset(&src, 0, sizeof (src));
  src.sin_family = AF_INET;
  src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  src.sin_port = htons(1);
  return (chanBlbTrnFdDatagramInject(
    (const struct sockaddr *)dst, sizeof (*dst),
    (const struct sockaddr *)&src, sizeof (src),
    dg, len));
}

/*
 * Inject one datagram of one fragment, hashBytes/hashLen its trailing
 * gate (0 to omit): tests of the gate's refusal pass bad bytes.
 */
static int
injectFrag(
  const struct sockaddr_in *dst
 ,const unsigned char *tagBytes
 ,unsigned int tagSize
 ,unsigned char kMinus1
 ,unsigned char mVal
 ,unsigned char si
 ,unsigned char padding
 ,const unsigned char *shardData
 ,unsigned char shardLen
 ,const unsigned char *hashBytes
 ,unsigned int hashLen
){
  unsigned char dg[512];
  unsigned int pos;

  pos = mkFrag(dg, tagBytes, tagSize, kMinus1, mVal, si, padding, shardData, shardLen);
  if (hashLen) {
    memcpy(dg + pos, hashBytes, hashLen);
    pos += hashLen;
  }
  return (injectDg(dst, dg, pos));
}

static void
testRmUmNoDecode(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "rmum test";
  unsigned int al;
  unsigned int off;

  printf("=== Test: rm/um bytes on direct delivery ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* k=1, m=3, no drop: all data shards present -> rm=3, um=0 */
  m = mkBlob(&env.sa, 2, 1, 3, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) {
    al = m->b[0];
    off = 1 + al + 2;
    check("rm == sender m (3)", m->b[off] == 3);
    check("um == 0 (all data present, no parity used)", m->b[off + 1] == 0);
    check("payload correct", verifyBlob(m, 2, msg, strlen(msg)));
    free(m);
  }
  teardown(&env, &rsecCtx);
}

static void
testRmUmWithDecode(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "decode um test abcdefghijkl";
  unsigned int al;
  unsigned int off;
  unsigned int tries;
  int sawUmGt0;
  int sawRmMatches;

  printf("=== Test: rm/um bytes when RS decode is used ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18;   /* small dgram => small shards: 27-byte msg spans a few */
  rsecCtx.tableSize = 8;

  chanBlbTrnFdDatagramDropPct = 40;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  sawUmGt0 = 0;
  sawRmMatches = 0;
  for (tries = 0; tries < 10; ++tries) {
    m = mkBlob(&env.sa, 2, (unsigned short)(200 + tries), 8, 0, msg, strlen(msg));
    sendBlob(env.outChan, m);
    m = recvBlob(env.inChan, 3000000000L);
    if (m) {
      al = m->b[0];
      off = 1 + al + 2;
      if (m->b[off] == 8) sawRmMatches = 1;
      if (m->b[off + 1] > 0) sawUmGt0 = 1;
      free(m);
    }
  }
  check("rm == 8 observed", sawRmMatches);
  check("um > 0 observed (parity consumed)", sawUmGt0);
  rsecSnap(&rsecCtx);
  check("igrDcd > 0", rsecCtx.igrDcd > 0);

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

static void
testIgrHashReject(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hashKeyCtx hk;
  struct hashKeyCtx hkI;
  chanBlb_t *m;
  unsigned char tag[2];
  unsigned char shard[3];
  unsigned char badHash[HALFSIPHASH_HSH_SZ];
  unsigned int i;

  printf("=== Test: igrHash increments on bad datagram MAC (halfsiphash) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;
  /* arbitrary 64-bit key for the test */
  for (i = 0; i < sizeof (hk.key); ++i)
    hk.key[i] = (unsigned char)(0xa5 ^ i);
  rsecCtx.dtgHashSize = HALFSIPHASH_HSH_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashSign = hashSignCb;
  rsecCtx.dtgHashVrfy = hashVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x11; tag[1] = 0x22;
  shard[0] = 'a'; shard[1] = 'b'; shard[2] = 'c';
  /* deliberately wrong trailing MAC: cannot be produced by anyone
   * who doesn't have the key, so hashVrfy must reject */
  for (i = 0; i < sizeof (badHash); ++i)
    badHash[i] = (unsigned char)(i * 7 + 3);
  check("inject bad-datagram-MAC frag",
    injectFrag(&env.sa,tag, 2, 0, 0, 0, 0, shard, 3,
               badHash, sizeof (badHash)));

  m = recvBlob(env.inChan, 500000000L);
  check("no delivery on bad datagram MAC", m == 0);
  if (m) free(m);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrHash >= 1", rsecCtx.igrHash >= 1);
  check("igrMsg == 0", rsecCtx.igrMsg == 0);

  teardown(&env, &rsecCtx);
}

static void
testHashMatch(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hashKeyCtx hk;
  struct hashKeyCtx hkI;
  chanBlb_t *m;
  const char *msg = "datagram GATE enabled";
  unsigned int i;

  printf("=== Test: datagram MAC (halfsiphash) round-trip ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;
  for (i = 0; i < sizeof (hk.key); ++i)
    hk.key[i] = (unsigned char)(0x5a ^ i);
  rsecCtx.dtgHashSize = HALFSIPHASH_HSH_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashSign = hashSignCb;
  rsecCtx.dtgHashVrfy = hashVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  rsecSnap(&rsecCtx);
  check("no datagram MAC failures", rsecCtx.igrHash == 0);
  teardown(&env, &rsecCtx);
}

static void
testIgrDupReject(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];
  unsigned char s0[3];
  unsigned char s1[3];
  unsigned int al;
  unsigned int off;

  printf("=== Test: igrDup increments on pre-delivery duplicate shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 12; /* shard 4: 3-byte shards at k=2 are a 5- or 6-byte message */
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x55; tag[1] = 0x66;
  s0[0] = 'A'; s0[1] = 'B'; s0[2] = 'C';
  s1[0] = 'D'; s1[1] = 'E'; s1[2] = 'F';
  /* k=2 (kMinus1=1) m=0: need both shards. Inject si=0 twice, then si=1. */
  check("inject shard 0",
    injectFrag(&env.sa,tag, 2, 1, 0, 0, 0, s0, 3, 0, 0));
  usleep(50000);
  check("inject shard 0 duplicate",
    injectFrag(&env.sa,tag, 2, 1, 0, 0, 0, s0, 3, 0, 0));
  usleep(50000);
  check("inject shard 1",
    injectFrag(&env.sa,tag, 2, 1, 0, 1, 0, s1, 3, 0, 0));

  m = recvBlob(env.inChan, 2000000000L);
  check("message delivered despite dup", m != 0);
  if (m) {
    al = m->b[0];
    off = 1 + al + 2 + 2;
    check("reassembled payload == ABCDEF",
      m->l == off + 6 && memcmp(m->b + off, "ABCDEF", 6) == 0);
    free(m);
  }

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrDup >= 1", rsecCtx.igrDup >= 1);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);

  teardown(&env, &rsecCtx);
}

static void
testParamMismatchLossNotif(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  chanBlb_t *loss;
  chanBlb_t *second;
  unsigned char tag[2];
  unsigned char s0a[3];
  unsigned char s0b[4];
  unsigned int al;
  unsigned int off;
  unsigned int i;

  printf("=== Test: parameter-mismatch eviction delivers loss notification ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 12; /* shard 4: 3-byte shards at k=2 are a 5- or 6-byte message */
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x77; tag[1] = 0x88;
  s0a[0] = 'P'; s0a[1] = 'Q'; s0a[2] = 'R';
  s0b[0] = 'X'; s0b[1] = 'Y'; s0b[2] = 'Z'; s0b[3] = '!';

  /* msg1: k=2 (kMinus1=1), m=0, shardSize=3.  Inject only shard 0 -> incomplete. */
  check("inject msg1 shard 0 (incomplete)",
    injectFrag(&env.sa,tag, 2, 1, 0, 0, 0, s0a, 3, 0, 0));
  usleep(80000);

  /* msg2: same tag, k=1 (kMinus1=0), m=0, shardSize=4.  Params mismatch. */
  check("inject msg2 shard 0 (param mismatch)",
    injectFrag(&env.sa,tag, 2, 0, 0, 0, 0, s0b, 4, 0, 0));

  loss = 0;
  second = 0;
  for (i = 0; i < 2; ++i) {
    m = recvBlob(env.inChan, 2000000000L);
    if (!m) continue;
    al = m->b[0];
    off = 1 + al + 2;  /* rm byte offset */
    /* loss notif: zero-length payload (total == prefixSize) with rm=0 */
    if (m->l == off + 2 && m->b[off] == 0)
      loss = m;
    else if (m->l == off + 2 + 4 && memcmp(m->b + off + 2, "XYZ!", 4) == 0)
      second = m;
    else
      free(m);
  }

  check("loss notification delivered", loss != 0);
  if (loss) {
    al = loss->b[0];
    off = 1 + al + 2;
    check("loss rm == 0", loss->b[off] == 0);
    check("loss um == received (1)", loss->b[off + 1] == 1);
    check("loss zero-length payload", loss->l == off + 2);
    free(loss);
  }
  check("second message delivered (XYZ!)", second != 0);
  if (second) free(second);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrEvict >= 1", rsecCtx.igrEvict >= 1);
  check("igrMsg >= 1 (second message)", rsecCtx.igrMsg >= 1);

  teardown(&env, &rsecCtx);
}

/*
 * Same tag, same k, m and shard size -- only the padding differs, so
 * the two payloads differ in length by fewer than k bytes.  Padding is
 * a parameter of the reassembly (it sets the delivered length), so a
 * mismatch must evict like any other: loss notification for the
 * incomplete first train, the second reassembled on its own.  Mixed,
 * the ingress would deliver six bytes neither sender sent.
 */
static void
testParamMismatchPadding(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  chanBlb_t *loss;
  chanBlb_t *second;
  unsigned char tag[2];
  unsigned char a0[3];
  unsigned char b0[3];
  unsigned char b1[3];
  unsigned int al;
  unsigned int off;
  unsigned int i;

  printf("=== Test: padding mismatch evicts, never mixes ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 12; /* shard 4: 3-byte shards at k=2 are a 5- or 6-byte message */
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x79; tag[1] = 0x8a;
  a0[0] = 'P'; a0[1] = 'Q'; a0[2] = 'R';
  b0[0] = 'A'; b0[1] = 'B'; b0[2] = 'C';
  b1[0] = 'D'; b1[1] = 'E'; b1[2] = 'F';

  /* msg A: k=2, m=0, shardSize=3, padding 0 (6 bytes).  Shard 0 only. */
  check("inject A shard 0 (incomplete)",
    injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, a0, 3, 0, 0));
  usleep(80000);
  /* msg B: k=2, m=0, shardSize=3, padding 1 (5 bytes).  Shard 1 first:
   * mixed with A's shard 0 it would complete as "PQRDEF". */
  check("inject B shard 1 (padding mismatch)",
    injectFrag(&env.sa, tag, 2, 1, 0, 1, 1, b1, 3, 0, 0));
  usleep(80000);
  check("inject B shard 0 (completes B)",
    injectFrag(&env.sa, tag, 2, 1, 0, 0, 1, b0, 3, 0, 0));

  loss = 0;
  second = 0;
  for (i = 0; i < 2; ++i) {
    m = recvBlob(env.inChan, 2000000000L);
    if (!m) continue;
    al = m->b[0];
    off = 1 + al + 2;
    if (m->l == off + 2 && m->b[off] == 0)
      loss = m;
    else if (m->l == off + 2 + 5 && memcmp(m->b + off + 2, "ABCDE", 5) == 0)
      second = m;
    else {
      printf("    delivered %u payload bytes: neither train's\n", m->l - (off + 2));
      free(m);
    }
  }
  check("loss notification for A", loss != 0);
  if (loss) {
    al = loss->b[0];
    off = 1 + al + 2;
    check("loss um == received (1)", loss->b[off + 1] == 1);
    free(loss);
  }
  check("B delivered as ABCDE", second != 0);
  if (second) free(second);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrEvict >= 1", rsecCtx.igrEvict >= 1);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);

  teardown(&env, &rsecCtx);
}

/*
 * Same tag, same k, same shardSize -- only m differs.
 * Confirms that m alone drives parameter-mismatch eviction.
 * Delivered-entry path (blob==NULL): silent evict, new entry collected,
 * message delivered a SECOND time with the new m.  This is the duplicate-
 * delivery risk callers with retransmit loops (e.g. AAB's ledger pump)
 * must account for if m can change between re-emissions of the same tag.
 */
static void
testSameTagMOnlyDelivered(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *first;
  chanBlb_t *second;
  unsigned char tag[2];
  unsigned char shard[3];
  unsigned int al;
  unsigned int off;

  printf("=== Test: same tag, m differs, original already delivered -> duplicate delivery ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x99; tag[1] = 0xAA;
  shard[0] = 'M'; shard[1] = 'N'; shard[2] = 'O';

  /* k=1 m=0 shardSize=3 -> completes on arrival */
  check("inject msg with m=0",
    injectFrag(&env.sa,tag, 2, 0, 0, 0, 0, shard, 3, 0, 0));
  first = recvBlob(env.inChan, 1000000000L);
  check("first delivery", first != 0);
  if (first) {
    al = first->b[0];
    off = 1 + al + 2;
    check("first rm == 0", first->b[off] == 0);
    free(first);
  }

  /* Same tag, same k, same shardSize, but m=5.  Param mismatch against
   * the delivered entry (blob==NULL) evicts silently, new entry collects
   * the shard, k=1 -> redelivered with m=5. */
  usleep(50000);
  check("inject same tag with m=5",
    injectFrag(&env.sa,tag, 2, 0, 5, 0, 0, shard, 3, 0, 0));
  second = recvBlob(env.inChan, 1000000000L);
  check("second (duplicate) delivery", second != 0);
  if (second) {
    al = second->b[0];
    off = 1 + al + 2;
    check("second rm == 5 (new m)", second->b[off] == 5);
    free(second);
  }

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrMsg == 2 (same message delivered twice)", rsecCtx.igrMsg == 2);
  check("igrEvict == 0 (silent evict for delivered entry)",
        rsecCtx.igrEvict == 0);

  teardown(&env, &rsecCtx);
}

/*
 * Same tag, same k, same shardSize -- only m differs, but the original
 * entry is still incomplete when the mismatched fragment arrives.
 * Fires igrEvict + loss notification (rm=0, um=received).
 */
static void
testSameTagMOnlyIncomplete(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  chanBlb_t *loss;
  unsigned char tag[2];
  unsigned char s0[3];
  unsigned int al;
  unsigned int off;
  unsigned int i;

  printf("=== Test: same tag, m differs, original incomplete -> loss notification ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 12; /* shard 4: 3-byte shards at k=2 are a 5- or 6-byte message */
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0xBB; tag[1] = 0xCC;
  s0[0] = 'P'; s0[1] = 'Q'; s0[2] = 'R';

  /* k=2 m=3 shardSize=3 -> incomplete after 1 shard */
  check("inject shard 0 (k=2 m=3, incomplete)",
    injectFrag(&env.sa,tag, 2, 1, 3, 0, 0, s0, 3, 0, 0));
  usleep(50000);

  /* Same tag, same k, same shardSize, different m (7).  m-only mismatch
   * still triggers param-mismatch eviction; the original is incomplete
   * (blob != NULL) so igrEvict++ and a loss notification is delivered. */
  check("inject shard 0 with different m (7)",
    injectFrag(&env.sa,tag, 2, 1, 7, 0, 0, s0, 3, 0, 0));

  loss = 0;
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 1000000000L);
    if (!m) continue;
    al = m->b[0];
    off = 1 + al + 2;
    if (m->l == off + 2 && m->b[off] == 0) {
      loss = m;
      break;
    }
    free(m);
  }

  check("loss notification delivered", loss != 0);
  if (loss) {
    al = loss->b[0];
    off = 1 + al + 2;
    check("loss rm == 0", loss->b[off] == 0);
    check("loss um == 1", loss->b[off + 1] == 1);
    free(loss);
  }
  rsecSnap(&rsecCtx);
  check("igrEvict >= 1 (m-only change evicts incomplete)",
        rsecCtx.igrEvict >= 1);

  teardown(&env, &rsecCtx);
}

/* ===== sender, geometry, straggler, eviction and configuration rules ===== */

static const unsigned char Abc[] = "abc";
static const unsigned char Def[] = "def";

/* nothing delivered within 300 ms */
static int
recvNone(
  chan_t *inChan
){
  chanBlb_t *m;

  if (!(m = recvBlob(inChan, 300000000L)))
    return (1);
  free(m);
  return (0);
}

/* a loss notification under tag t0 t1 holding um fragments */
static int
isLoss(
  const chanBlb_t *m
 ,unsigned char t0
 ,unsigned char t1
 ,unsigned char um
){
  unsigned int al;

  al = m->b[0];
  return (m->l == 1 + al + 2 + 2
       && m->b[1 + al] == t0 && m->b[1 + al + 1] == t1
       && !m->b[1 + al + 2] && m->b[1 + al + 3] == um);
}

/*
 * tagOwner = 1: every fragment of a datagram names the first's sender.
 * One datagram of three single-shard fragments, the second naming
 * another sender: the first delivered, the second refused and counted
 * in hash, and the third, its first sender's again, with it.  Two under
 * one sender: both delivered.
 */
static void
testTagOwnerUniformity(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char dg[64];
  unsigned char t0[2];
  unsigned char t1[2];
  unsigned char t2[2];
  unsigned int n;

  printf("=== Test: tagOwner, a fragment naming another sender refused ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 8;
  rsecCtx.tagOwner = 1;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  t0[0] = 1; t0[1] = 0x10;
  t1[0] = 2; t1[1] = 0x11;
  t2[0] = 1; t2[1] = 0x14;
  n = mkFrag(dg, t0, 2, 0, 0, 0, 0, Abc, 3);
  n += mkFrag(dg + n, t1, 2, 0, 0, 0, 0, Def, 3);
  n += mkFrag(dg + n, t2, 2, 0, 0, 0, 0, Abc, 3);
  check("inject a datagram naming two senders", injectDg(&env.sa, dg, n));
  m = recvBlob(env.inChan, 1000000000L);
  check("the first sender's fragment delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  check("the other sender's fragment refused, and the rest after it", recvNone(env.inChan));

  t0[1] = 0x12;
  t1[0] = 1; t1[1] = 0x13;
  n = mkFrag(dg, t0, 2, 0, 0, 0, 0, Abc, 3);
  n += mkFrag(dg + n, t1, 2, 0, 0, 0, 0, Def, 3);
  check("inject a datagram naming one sender", injectDg(&env.sa, dg, n));
  m = recvBlob(env.inChan, 1000000000L);
  check("its first delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  m = recvBlob(env.inChan, 1000000000L);
  check("its second delivered", m && verifyBlob(m, 2, "def", 3));
  if (m) free(m);

  rsecSnap(&rsecCtx);
  check("igrHash == 1", rsecCtx.igrHash == 1);
  check("igrMsg == 3", rsecCtx.igrMsg == 3);
  teardown(&env, &rsecCtx);
}

/*
 * tagOwner on a full table: a new tag evicts from the sender holding the
 * most entries.  Table 4, k=2 reassemblies: sender 2 opens one first,
 * the oldest, then sender 1 three; sender 2's second tag evicts sender
 * 1's oldest, a loss under sender 1, and sender 2's first still
 * completes.
 */
static void
testTagOwnerEviction(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];
  unsigned int i;

  printf("=== Test: tagOwner, a full table evicts from the sender holding the most ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3: a 6-byte message is k=2 */
  rsecCtx.tableSize = 4;
  rsecCtx.tagOwner = 1;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 2; tag[1] = 0x20;
  check("inject sender 2's first", injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0));
  tag[0] = 1;
  for (i = 0; i < 3; ++i) {
    tag[1] = 0x21 + i;
    injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  }
  tag[0] = 2; tag[1] = 0x24;
  check("inject sender 2's second", injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0));
  m = recvBlob(env.inChan, 1000000000L);
  check("a loss of sender 1's oldest", m && isLoss(m, 1, 0x21, 1));
  if (m) free(m);
  tag[1] = 0x20;
  check("inject sender 2's first, its second shard", injectFrag(&env.sa, tag, 2, 1, 0, 1, 0, Def, 3, 0, 0));
  m = recvBlob(env.inChan, 1000000000L);
  check("sender 2's first delivered", m && verifyBlob(m, 2, "abcdef", 6));
  if (m) free(m);

  rsecSnap(&rsecCtx);
  check("igrEvict == 1", rsecCtx.igrEvict == 1);
  check("igrEvictFull == 1", rsecCtx.igrEvictFull == 1);
  teardown(&env, &rsecCtx);
}

/*
 * k and the shard are the message length's: k = ceil(L / shard), the
 * padding under k.  At shard 3: k=1 of 3 bytes, k=2 of 5, and an empty
 * message delivered; k=2 of a 1-byte shard (2 bytes is k=1), a padding
 * of k, and k=3 of 2-byte shards (6 bytes is k=2) refused; and a
 * fragment of k + m past 256, and one cut short of its shard, counted
 * the same way.
 */
static void
testGeometryRefused(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: a geometry no egress derives refused (igrShape) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3 */
  rsecCtx.tableSize = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[1] = 0;
  tag[0] = 0x30;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("k=1 of 3 bytes delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);

  tag[0] = 0x31;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 1, 0, 0);
  tag[0] = 0x32;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 1, Abc, 2, 0, 0);
  tag[0] = 0x33;
  injectFrag(&env.sa, tag, 2, 2, 0, 0, 0, Abc, 2, 0, 0);
  check("none of the three delivered", recvNone(env.inChan));

  tag[0] = 0x34;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 1, Abc, 3, 0, 0);
  injectFrag(&env.sa, tag, 2, 1, 0, 1, 1, Def, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("k=2 of 5 bytes delivered", m && verifyBlob(m, 2, "abcde", 5));
  if (m) free(m);

  tag[0] = 0x35;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 0, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("the empty message delivered", m && verifyBlob(m, 2, "", 0));
  if (m) free(m);

  tag[0] = 0x36;
  injectFrag(&env.sa, tag, 2, 255, 1, 0, 0, Abc, 3, 0, 0);
  tag[0] = 0x37;
  {
    unsigned char dg[16];

    injectDg(&env.sa, dg, mkFrag(dg, tag, 2, 0, 0, 0, 0, Abc, 3) - 1);
  }
  check("neither delivered", recvNone(env.inChan));

  rsecSnap(&rsecCtx);
  check("igrShape == 5", rsecCtx.igrShape == 5);
  check("igrMsg == 3", rsecCtx.igrMsg == 3);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  teardown(&env, &rsecCtx);
}

/* msgMax 4 at shard 3: a 4-byte message delivered, a 5-byte one refused */
static void
testMsgMax(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: msgMax refuses a fragment implying a longer message ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3 */
  rsecCtx.tableSize = 8;
  rsecCtx.msgMax = 4;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x40; tag[1] = 0;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 2, 0, 0);
  injectFrag(&env.sa, tag, 2, 1, 0, 1, 0, Def, 2, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("4 bytes delivered", m && verifyBlob(m, 2, "abde", 4));
  if (m) free(m);

  tag[0] = 0x41;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 1, Abc, 3, 0, 0);
  injectFrag(&env.sa, tag, 2, 1, 0, 1, 1, Def, 3, 0, 0);
  check("5 bytes refused", recvNone(env.inChan));

  rsecSnap(&rsecCtx);
  check("igrShape == 2", rsecCtx.igrShape == 2);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  teardown(&env, &rsecCtx);
}

/*
 * A datagram past dgramMax is not of this wire: dropped uncounted, even
 * with a whole fragment in it.  At dgramMax 11, a 10-byte fragment and
 * two bytes more is dropped; with one byte more it is delivered.
 */
static void
testOversizeDatagram(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char dg[64];
  unsigned char tag[2];
  unsigned int n;

  printf("=== Test: a datagram past dgramMax dropped ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11;
  rsecCtx.tableSize = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x50; tag[1] = 0;
  n = mkFrag(dg, tag, 2, 0, 0, 0, 0, Abc, 3);
  dg[n] = dg[n + 1] = 0;
  injectDg(&env.sa, dg, n + 2);
  check("12 bytes dropped", recvNone(env.inChan));
  rsecSnap(&rsecCtx);
  check("uncounted (igrFrg == 0)", rsecCtx.igrFrg == 0);

  injectDg(&env.sa, dg, n + 1);
  m = recvBlob(env.inChan, 1000000000L);
  check("11 bytes delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  rsecSnap(&rsecCtx);
  check("igrFrg == 1", rsecCtx.igrFrg == 1);
  teardown(&env, &rsecCtx);
}

/*
 * A delivered train eats its stragglers until k + m DISTINCT fragments
 * have come: copies count toward nothing.  k=1 m=2: shard 0 delivers;
 * two copies of it and shard 1 are late, no second delivery; shard 2
 * ends the train; shard 0 again is a re-send, delivered again.
 */
static void
testStragglerCopies(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: a straggler's copies do not end its train ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11;
  rsecCtx.tableSize = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 0x60; tag[1] = 0;
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("shard 0 delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  injectFrag(&env.sa, tag, 2, 0, 2, 1, 0, Def, 3, 0, 0);
  check("two copies and shard 1: no second delivery", recvNone(env.inChan));
  injectFrag(&env.sa, tag, 2, 0, 2, 2, 0, Def, 3, 0, 0);
  check("shard 2: no second delivery", recvNone(env.inChan));
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("then a re-send delivered again", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);

  rsecSnap(&rsecCtx);
  check("igrLate == 4", rsecCtx.igrLate == 4);
  check("igrMsg == 2", rsecCtx.igrMsg == 2);
  teardown(&env, &rsecCtx);
}

/*
 * A new tag on a full table, table 2: an entry whose train has all come
 * goes first, then a delivered one, and one in flight last, older or
 * not.  P (k=1 m=1) delivered with its parity to come, then S (k=1 m=0)
 * delivered and spent: Q evicts S though P is older, so P's parity is
 * still its straggler.  A (k=2) in flight, B (k=1 m=0) delivered and
 * spent: C evicts B, and A completes.  C (k=2) in flight, D (k=1 m=1)
 * delivered with its parity to come: E evicts D though C is older, and
 * C completes.  No loss.
 */
static void
testSpentEvictedFirst(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: a full table evicts spent, then delivered, then in flight ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3 */
  rsecCtx.tableSize = 2;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[1] = 0;
  tag[0] = 0x6d;
  injectFrag(&env.sa, tag, 2, 0, 1, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("P delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  tag[0] = 0x6e;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("S delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  tag[0] = 0x6f;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("Q delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  tag[0] = 0x6d;
  injectFrag(&env.sa, tag, 2, 0, 1, 1, 0, Def, 3, 0, 0);
  check("P's parity a straggler, not a new train", recvNone(env.inChan));

  tag[0] = 0x70;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[0] = 0x71;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("B delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  tag[0] = 0x72;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[0] = 0x70;
  injectFrag(&env.sa, tag, 2, 1, 0, 1, 0, Def, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("A delivered, not evicted", m && verifyBlob(m, 2, "abcdef", 6));
  if (m) free(m);

  tag[0] = 0x73;
  injectFrag(&env.sa, tag, 2, 0, 1, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("D delivered", m && verifyBlob(m, 2, "abc", 3));
  if (m) free(m);
  tag[0] = 0x74;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[0] = 0x72;
  injectFrag(&env.sa, tag, 2, 1, 0, 1, 0, Def, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("C delivered, not evicted", m && verifyBlob(m, 2, "abcdef", 6));
  if (m) free(m);

  rsecSnap(&rsecCtx);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  check("igrMsg == 7", rsecCtx.igrMsg == 7);
  teardown(&env, &rsecCtx);
}

/*
 * A configuration the framer cannot run ends its thread at once, which
 * shuts its channel.  The check waits on the channel's shut, a second at
 * most: a framer that runs never shuts it.
 */
static int
shutWithin(
  chan_t *c
){
  chanArr_t p[1];

  p[0].c = c;
  p[0].v = 0;
  p[0].o = chanOpSht;
  return (chanOne(1000000000L, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsSht);
}

static void
testConfigRefused(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hashKeyCtx hk;
  struct hashKeyCtx hkI;

  printf("=== Test: a configuration the framer cannot run shuts its channel ===\n");
  memset(&hk, 0, sizeof (hk));
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 8;

  rsecCtx.tagOwner = 3;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }
  check("tagOwner past the tag: ingress shut", shutWithin(env.inChan));
  check("tagOwner past the tag: egress runs", !shutWithin(env.outChan));
  teardown(&env, &rsecCtx);
  rsecCtx.tagOwner = 0;

  rsecCtx.dgramMax = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }
  check("no shard fits: ingress shut", shutWithin(env.inChan));
  check("no shard fits: egress shut", shutWithin(env.outChan));
  teardown(&env, &rsecCtx);
  rsecCtx.dgramMax = 508;

  rsecCtx.tableSize = 0;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }
  check("no table: ingress shut", shutWithin(env.inChan));
  check("no table: egress shut", shutWithin(env.outChan));
  teardown(&env, &rsecCtx);
  rsecCtx.tableSize = 8;

  rsecCtx.dtgHashSize = HALFSIPHASH_HSH_SZ;
  hkI = hk;
  rsecCtx.egrHashCtx = &hk;
  rsecCtx.igrHashCtx = &hkI;
  rsecCtx.dtgHashVrfy = hashVrfyCb;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }
  check("a gate without its sign: egress shut", shutWithin(env.outChan));
  check("a gate without its sign: ingress runs", !shutWithin(env.inChan));
  teardown(&env, &rsecCtx);

  rsecCtx.dtgHashSign = hashSignCb;
  rsecCtx.dtgHashVrfy = 0;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }
  check("a gate without its verify: ingress shut", shutWithin(env.inChan));
  check("a gate without its verify: egress runs", !shutWithin(env.outChan));
  teardown(&env, &rsecCtx);
}

/*
 * tagOwner, a tie for the most entries: the incoming sender's own goes.
 * Table 4: sender 1 opens two reassemblies, the oldest, then sender 2
 * two; sender 2's third tag evicts sender 2's oldest, not sender 1's.
 */
static void
testTagOwnerTie(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: tagOwner, a tie evicts the incoming sender's own ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3: a 6-byte message is k=2 */
  rsecCtx.tableSize = 4;
  rsecCtx.tagOwner = 1;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[0] = 1; tag[1] = 0x80;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[1] = 0x81;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[0] = 2; tag[1] = 0x82;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[1] = 0x83;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  tag[1] = 0x84;
  injectFrag(&env.sa, tag, 2, 1, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("a loss of sender 2's oldest", m && isLoss(m, 2, 0x82, 1));
  if (m) free(m);
  rsecSnap(&rsecCtx);
  check("igrEvict == 1", rsecCtx.igrEvict == 1);
  teardown(&env, &rsecCtx);
}

/*
 * A late fragment does not feed its entry's age: a delivered entry ages
 * from its delivery.  Table 2: D1 then D2 (k=1 m=2) delivered, D1's
 * parity late; E's tag evicts D1, the older by its feeding, so D2's
 * parity is still a straggler.
 */
static void
testLateNoAge(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char tag[2];

  printf("=== Test: a late fragment does not feed its entry's age ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11;
  rsecCtx.tableSize = 2;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  tag[1] = 0;
  tag[0] = 0x90;
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("D1 delivered", m != 0);
  if (m) free(m);
  tag[0] = 0x91;
  injectFrag(&env.sa, tag, 2, 0, 2, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("D2 delivered", m != 0);
  if (m) free(m);
  tag[0] = 0x90;
  injectFrag(&env.sa, tag, 2, 0, 2, 1, 0, Def, 3, 0, 0);
  check("D1's parity late", recvNone(env.inChan));
  tag[0] = 0x92;
  injectFrag(&env.sa, tag, 2, 0, 0, 0, 0, Abc, 3, 0, 0);
  m = recvBlob(env.inChan, 1000000000L);
  check("E delivered", m != 0);
  if (m) free(m);
  tag[0] = 0x91;
  injectFrag(&env.sa, tag, 2, 0, 2, 1, 0, Def, 3, 0, 0);
  check("D2's parity still a straggler", recvNone(env.inChan));

  rsecSnap(&rsecCtx);
  check("igrLate == 2", rsecCtx.igrLate == 2);
  check("igrMsg == 3", rsecCtx.igrMsg == 3);
  teardown(&env, &rsecCtx);
}

/*
 * What the egress drops: a null item and a blob past chanBlbChnRsecMax
 * at m = 0; the next blob is taken either way.
 */
static void
testEgressDropped(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned char *big;
  unsigned int bigLen;

  printf("=== Test: a null item and a blob past the max dropped, the next taken ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 11; /* shard 3, max 768 */
  rsecCtx.tableSize = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  check("send a null item", sendBlob(env.outChan, 0));
  m = mkBlob(&env.sa, 2, 1, 0, 0, "after", 5);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 1000000000L);
  check("the blob after the null item delivered", m && verifyBlob(m, 2, "after", 5));
  if (m) free(m);

  bigLen = chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.dtgHashSize, 0) + 1;
  if ((big = calloc(1, bigLen))) {
    m = mkBlob(&env.sa, 2, 2, 0, 0, big, bigLen);
    free(big);
    check("send one byte past the max", sendBlob(env.outChan, m));
  }
  m = mkBlob(&env.sa, 2, 3, 0, 0, "after", 5);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("the blob after it delivered", m && verifyBlob(m, 2, "after", 5));
  if (m) free(m);
  check("nothing else", recvNone(env.inChan));
  rsecSnap(&rsecCtx);
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  teardown(&env, &rsecCtx);
}

/*
 * The egress channel shut right after its Puts: every message taken is
 * still sent, at its pace, before the framer ends, as a stream's close
 * sends what was written.
 */
static void
testShutDrain(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int got;

  printf("=== Test: the egress channel shut, what it took is still sent ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 8;
  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 4; ++i) {
    m = mkBlob(&env.sa, 2, 0xa0 + i, 2, 5000, "drain", 5);
    sendBlob(env.outChan, m);
  }
  chanShut(env.outChan);
  for (got = 0, i = 0; i < 4; ++i)
    if ((m = recvBlob(env.inChan, 2000000000L))) {
      got += verifyBlob(m, 2, "drain", 5);
      free(m);
    }
  check("all 4 delivered", got == 4);
  rsecSnap(&rsecCtx);
  check("igrMsg == 4", rsecCtx.igrMsg == 4);
  teardown(&env, &rsecCtx);
}

int
main(
  void
){
  Pass = 0;
  Fail = 0;

  chanBlbTrnFdDatagramDropPct = 0;
  chanBlbTrnFdDatagramDelayMs = 0;
  chanInit(realloc, free);

  /* basic tests (no pathology) */
  testBasicM1();
  testBasicM0();
  testDedupM2();
  testDedupExpiry();
  testMultiShard();
  testEmptyPayload();
  testSequential();
  testTinyShard();
  testVlq2Byte();
  testVlqMaxShard();
  testDgramMaxValidation();
  testHighParity();

  /* max payload tests */
  testMaxApi();
  testMaxBoundary();

  /* drop tests */
  testDropRecovery();
  testDropMultiShard();
  testDropMultipleMessages();

  /* delay (reorder) tests */
  testDelayReorder();
  testDelaySequential();

  /* combined stress tests */
  testStressCombined();
  testStressMultiShardGate();

  /* counter tests */
  testCountersBasic();
  testCountersEgrFull();
  testCountersMultiShard();
  testCountersDrop();
  testCountersDedup();

  /* shard size API */
  testShardApi();

  /* egress pacing tests */
  testEgressInterleave();
  testEgressBackpressure();
  testCountersBackpressure();

  /* coverage gap tests */
  testRsDecodeAliasing();
  testMClampIntermediate();
  testMalformedBlob();
  testEmptyPayloadDrop();
  testTagSizeZero();
  testLargeK();
  testGateHmacRsDecode();

  /* packing tests */
  testPackSmall();
  testPackMixedSize();
  testPackDrop();
  testPackOwnerGate();
  testPackCounters();
  testPackSameMessage();
  testPackSameTag();

  /* pacing tests */
  testPaceSingleMessage();
  testPacePackingDensity();
  testPaceMaxDelayMixed();
  testPaceNoBlasting();

  /* rm/um + raw-injection coverage */
  testRmUmNoDecode();
  testRmUmWithDecode();
  testIgrHashReject();
  testHashMatch();
  testIgrDupReject();
  testParamMismatchLossNotif();
  testParamMismatchPadding();
  testSameTagMOnlyDelivered();
  testSameTagMOnlyIncomplete();

  /* sender, geometry, straggler, eviction and configuration rules */
  testTagOwnerUniformity();
  testTagOwnerEviction();
  testGeometryRefused();
  testMsgMax();
  testOversizeDatagram();
  testStragglerCopies();
  testSpentEvictedFirst();
  testTagOwnerTie();
  testLateNoAge();
  testConfigRefused();
  testEgressDropped();
  testShutDrain();

  printf("\n=======================================\n");
  printf("Results: %d passed, %d failed\n", Pass, Fail);
  printf("=======================================\n");
  return (Fail > 0 ? 1 : 0);
}
