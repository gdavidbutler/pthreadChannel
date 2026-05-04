/*
 * Unit test for chanBlbChnRsec
 * Uses loopback UDP self-send with link-time transport replacement.
 * Link against chanBlbTrnFdDatagramStress.c for drop+delay simulation.
 */

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

/* milliseconds between two observed packet timestamps (b - a) */
static long
obsDeltaMs(
  unsigned int a
 ,unsigned int b
){
  long s;
  long n;

  s = (long)(chanBlbTrnFdDatagramObs[b].ts.tv_sec
           - chanBlbTrnFdDatagramObs[a].ts.tv_sec);
  n = chanBlbTrnFdDatagramObs[b].ts.tv_nsec
    - chanBlbTrnFdDatagramObs[a].ts.tv_nsec;
  return (s * 1000 + n / 1000000);
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

/* HMAC callbacks */
struct hmacKeyCtx {
  const unsigned char *key;
  unsigned int keyLen;
};

static void
hmacSignCb(
  void *ctx
 ,const unsigned char *hdr
 ,unsigned char *dst
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacKeyCtx *k;

  (void)hdr;
  k = (struct hmacKeyCtx *)ctx;
  rmd128hmac(k->key, k->keyLen, src, len, dst);
}

static int
hmacVrfyCb(
  void *ctx
 ,const unsigned char *hdr
 ,const unsigned char *mac
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacKeyCtx *k;
  unsigned char computed[RMD128_SZ];
  unsigned char diff;
  unsigned int i;

  (void)hdr;
  k = (struct hmacKeyCtx *)ctx;
  rmd128hmac(k->key, k->keyLen, src, len, computed);
  diff = 0;
  for (i = 0; i < RMD128_SZ; ++i)
    diff |= mac[i] ^ computed[i];
  return (diff == 0);
}

/* Encrypt/Decrypt callbacks (XOR stream cipher for testing) */
struct cryptKeyCtx {
  const unsigned char *key;
  unsigned int keyLen;
};

static void
xorCryptCb(
  void *ctx
 ,const unsigned char *hdr
 ,unsigned char *data
 ,unsigned int len
){
  struct cryptKeyCtx *k;
  unsigned int i;

  (void)hdr;
  k = (struct cryptKeyCtx *)ctx;
  for (i = 0; i < len; ++i)
    data[i] ^= k->key[i % k->keyLen];
}

/* hdr-dependent HMAC callbacks - derive key by XORing with tag from hdr */
struct hmacHdrCtx {
  const unsigned char *key;
  unsigned int keyLen;
  unsigned int tagSize;
};

static void
hmacHdrSignCb(
  void *ctx
 ,const unsigned char *hdr
 ,unsigned char *dst
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacHdrCtx *k;
  unsigned char dk[64];
  unsigned int tagOff;
  unsigned int dkLen;
  unsigned int i;

  k = (struct hmacHdrCtx *)ctx;
  dkLen = k->keyLen < sizeof (dk) / sizeof (dk[0])
        ? k->keyLen : (unsigned int)(sizeof (dk) / sizeof (dk[0]));
  tagOff = 1 + (unsigned int)hdr[0];
  for (i = 0; i < dkLen; ++i)
    dk[i] = k->key[i] ^ hdr[tagOff + i % k->tagSize];
  rmd128hmac(dk, dkLen, src, len, dst);
}

static int
hmacHdrVrfyCb(
  void *ctx
 ,const unsigned char *hdr
 ,const unsigned char *mac
 ,const unsigned char *src
 ,unsigned int len
){
  struct hmacHdrCtx *k;
  unsigned char dk[64];
  unsigned char computed[RMD128_SZ];
  unsigned char diff;
  unsigned int tagOff;
  unsigned int dkLen;
  unsigned int i;

  k = (struct hmacHdrCtx *)ctx;
  dkLen = k->keyLen < sizeof (dk) / sizeof (dk[0])
        ? k->keyLen : (unsigned int)(sizeof (dk) / sizeof (dk[0]));
  tagOff = 1 + (unsigned int)hdr[0];
  for (i = 0; i < dkLen; ++i)
    dk[i] = k->key[i] ^ hdr[tagOff + i % k->tagSize];
  rmd128hmac(dk, dkLen, src, len, computed);
  diff = 0;
  for (i = 0; i < RMD128_SZ; ++i)
    diff |= mac[i] ^ computed[i];
  return (diff == 0);
}

/* hdr-dependent encrypt/decrypt - derive key by XORing with tag from hdr */
struct cryptHdrCtx {
  const unsigned char *key;
  unsigned int keyLen;
  unsigned int tagSize;
};

static void
xorHdrCryptCb(
  void *ctx
 ,const unsigned char *hdr
 ,unsigned char *data
 ,unsigned int len
){
  struct cryptHdrCtx *k;
  unsigned char dk[64];
  unsigned int tagOff;
  unsigned int dkLen;
  unsigned int i;

  k = (struct cryptHdrCtx *)ctx;
  dkLen = k->keyLen < sizeof (dk) / sizeof (dk[0])
        ? k->keyLen : (unsigned int)(sizeof (dk) / sizeof (dk[0]));
  tagOff = 1 + (unsigned int)hdr[0];
  for (i = 0; i < dkLen; ++i)
    dk[i] = k->key[i] ^ hdr[tagOff + i % k->tagSize];
  for (i = 0; i < len; ++i)
    data[i] ^= dk[i % dkLen];
}

/*
 * Build an RSEC egress blob:
 * [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][delay_ms(1)][payload]
 */
static chanBlb_t *
mkBlob(
  const struct sockaddr_in *sa
 ,unsigned int tagSize
 ,unsigned short tag
 ,unsigned char mVal
 ,unsigned char delayMs
 ,const void *payload
 ,unsigned int payloadLen
){
  unsigned char al;
  unsigned int bl;
  unsigned int off;
  unsigned int i;
  chanBlb_t *m;

  al = (unsigned char)sizeof (*sa);
  bl = 1 + al + tagSize + 2 + payloadLen;
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
  m->b[off + 1] = delayMs;
  if (payloadLen > 0)
    memcpy(m->b + off + 2, payload, payloadLen);
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
  /* config — set by the test before setup() */
  unsigned int tagSize;
  unsigned int dgramMax;
  unsigned int tableSize;
  unsigned int hmacSize;
  void *hmacCtx;
  void (*hmacSign)(void *, const unsigned char *, unsigned char *, const unsigned char *, unsigned int);
  int  (*hmacVrfy)(void *, const unsigned char *, const unsigned char *, const unsigned char *, unsigned int);
  void *cryptCtx;
  void (*encrypt)(void *, const unsigned char *, unsigned char *, unsigned int);
  void (*decrypt)(void *, const unsigned char *, unsigned char *, unsigned int);
  /* counter snapshot — populated by rsecSnap() */
  unsigned int egrMsg, egrFrg;
  unsigned int igrFrg, igrHash, igrHmac, igrDup, igrLate, igrMsg, igrDcd, igrEvict;
  /* private — populated by setup() */
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
  p->igrFrg = ic.frg;
  p->igrHash = ic.hash;
  p->igrHmac = ic.hmac;
  p->igrDup = ic.dup;
  p->igrLate = ic.late;
  p->igrMsg = ic.msg;
  p->igrDcd = ic.dcd;
  p->igrEvict = ic.evict;
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

  /* copy shared config into split contexts (opaque stays zero — the
   * framer threads allocate / free private state on entry / exit) */
  rsecCtx->egr.dgramMax  = rsecCtx->igr.dgramMax  = rsecCtx->dgramMax;
  rsecCtx->egr.tagSize   = rsecCtx->igr.tagSize   = rsecCtx->tagSize;
  rsecCtx->egr.tableSize = rsecCtx->igr.tableSize = rsecCtx->tableSize;
  rsecCtx->egr.hmacSize  = rsecCtx->igr.hmacSize  = rsecCtx->hmacSize;
  rsecCtx->egr.hmacCtx   = rsecCtx->igr.hmacCtx   = rsecCtx->hmacCtx;
  rsecCtx->egr.hmacSign  = rsecCtx->hmacSign;
  rsecCtx->igr.hmacVrfy  = rsecCtx->hmacVrfy;
  rsecCtx->egr.cryptCtx  = rsecCtx->igr.cryptCtx  = rsecCtx->cryptCtx;
  rsecCtx->egr.encrypt   = rsecCtx->encrypt;
  rsecCtx->igr.decrypt   = rsecCtx->decrypt;
  rsecCtx->egr.opaque    = 0;
  rsecCtx->igr.opaque    = 0;

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
  return (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsPut);
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

static void
testMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard message requiring several fragments";

  printf("=== Test: multi-shard (dgramMax=16, shardSize=6) ===\n");
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
testHmacMatch(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hmacCtx;
  chanBlb_t *m;
  const char *msg = "hmac authenticated";
  const char *key = "secret123";

  printf("=== Test: HMAC matching keys ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 536;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testHmacMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hmacCtx;
  chanBlb_t *m;
  const char *msg = "hmac multi shard test message payload";
  const char *key = "multikey";

  printf("=== Test: HMAC multi-shard (dgramMax=40 m=2) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 40;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 2, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  m = recvBlob(env.inChan, 500000000L);
  check("no duplicate", m == 0);
  if (m) free(m);
  teardown(&env, &rsecCtx);
}

static void
testTinyShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "stress test with tiny shards yep";

  printf("=== Test: tiny dgramMax=12 shardSize=2 m=2 (many fragments) ===\n");
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

  printf("=== Test: 2-byte VLQ padding (dgramMax=266, 1-byte payload) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 266; /* overhead=10, shardSize=256, 1-byte payload => padding=255 (2-byte VLQ) */
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

  printf("=== Test: VLQ max shard (dgramMax=16394, shardSize=16384) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16394; /* overhead=10, shardSize=16384, padding=16383 */
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 1-byte payload, shardSize=16384 => padding=16383 (max 2-byte VLQ) */
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
  struct rsecPair rsecCtx;

  printf("=== Test: dgramMax validation (VLQ limit) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;

  /* overhead=2+0+8=10, max shardSize=16384, max dgramMax=16394 */
  rsecCtx.dgramMax = 16394;
  check("16394 shard == 16384", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 16384);
  check("16394 max > 0", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 1) > 0);

  rsecCtx.dgramMax = 16395;
  check("16395 shard == 0", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 0);
  check("16395 max == 0", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 1) == 0);

  /* with HMAC: overhead=2+16+8=26, max dgramMax=16410 */
  rsecCtx.hmacSize = 16;
  rsecCtx.dgramMax = 16410;
  check("hmac 16410 shard == 16384", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 16384);

  rsecCtx.dgramMax = 16411;
  check("hmac 16411 shard == 0", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 0);
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
  struct rsecPair rsecCtx;

  printf("=== Test: chanBlbChnRsecMax API ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520; /* overhead=2+0+8=10, shardSize=510 */

  check("m=0 => 256*510=130560", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 0) == 130560);
  check("m=1 => 255*510=130050", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 1) == 130050);
  check("m=5 => 251*510=128010", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 5) == 128010);
  check("m=255 => 1*510=510", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 255) == 510);

  rsecCtx.dgramMax = 16; /* overhead=10, shardSize=6 */
  check("d=16 m=2 => 254*6=1524", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 2) == 1524);

  rsecCtx.dgramMax = 10; /* overhead=10, shardSize=0: too small */
  check("dgramMax==overhead => 0", chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 0) == 0);
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

  maxLen = chanBlbChnRsecMax(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize, 1); /* 255 * 6 = 1530 */
  check("maxLen == 1530", maxLen == 1530);

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
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8 */
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
testStressMultiShardHmac(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hmacCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[5];
  const char *key = "stresskey";
  const char *msgs[5];

  msgs[0] = "hmac stress first message here";
  msgs[1] = "hmac stress second message go";
  msgs[2] = "hmac stress third message ok";
  msgs[3] = "hmac stress fourth one too!";
  msgs[4] = "hmac stress fifth final msg";

  printf("=== Test: 15%% drop + 30ms delay, multi-shard dgramMax=42 m=6, HMAC ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 42; /* overhead=26, shardSize=16 */
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;

  chanBlbTrnFdDatagramDropPct = 15;
  chanBlbTrnFdDatagramDelayMs = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); goto stressHmacDone; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 6, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto stressHmacTeardown; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 5000000000L);
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
  check("all 5 HMAC multi-shard messages survived stress (any order)", ok == 5);

stressHmacTeardown:
  teardown(&env, &rsecCtx);
stressHmacDone:
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
  check("igrFrg == 2", rsecCtx.igrFrg == 2);
  check("igrHash == 0", rsecCtx.igrHash == 0);
  check("igrHmac == 0", rsecCtx.igrHmac == 0);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDcd == 0", rsecCtx.igrDcd == 0);
  check("igrDup == 0", rsecCtx.igrDup == 0);
  check("igrLate > 0", rsecCtx.igrLate > 0);
  check("igrEvict == 0", rsecCtx.igrEvict == 0);
  teardown(&env, &rsecCtx);
}

static void
testCountersMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard counter test msg!";

  printf("=== Test: counters multi-shard s=6 m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 28 bytes / 6 = 5 data shards, m=1 => 6 fragments */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 6", rsecCtx.egrFrg == 6);
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

  printf("=== Test: counters with 20%% drop, multi-shard s=8 m=8 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8 */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* 4 data, m=8 => 12 fragments sent, need 4 */
  m = mkBlob(&env.sa, 2, 1, 8, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  rsecSnap(&rsecCtx);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 12", rsecCtx.egrFrg == 12);
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
  struct rsecPair rsecCtx;

  printf("=== Test: chanBlbChnRsecShard API ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520; /* overhead=2+0+8=10, shardSize=510 */

  check("shard=510", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 510);

  rsecCtx.hmacSize = 16; /* overhead=2+16+8=26, shardSize=494 */
  check("shard=494 with hmac", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 494);

  rsecCtx.dgramMax = 16;
  rsecCtx.hmacSize = 0; /* overhead=2+0+8=10, shardSize=6 */
  check("shard=6 small dgram", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 6);

  rsecCtx.dgramMax = 10; /* overhead=10, shardSize=0: too small */
  check("dgramMax==overhead => 0", chanBlbChnRsecShard(rsecCtx.dgramMax, rsecCtx.tagSize, rsecCtx.hmacSize) == 0);
}

/* ===== ENCRYPT/DECRYPT TESTS ===== */

static void
testEncryptBasic(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypted hello";
  const char *key = "cryptkey42";

  printf("=== Test: encrypt basic m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;
  cryptCtx.key = (const unsigned char *)key;
  cryptCtx.keyLen = strlen(key);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testEncryptMultiShard(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypted multi shard message requiring several fragments";
  const char *key = "multishard-key";

  printf("=== Test: encrypt multi-shard dgramMax=16 m=2 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;
  cryptCtx.key = (const unsigned char *)key;
  cryptCtx.keyLen = strlen(key);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 2, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testEncryptHmac(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hmacCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypted and hmac authenticated";
  const char *hmacKey = "hmacSecret";
  const char *cryptKey = "cryptSecret";

  printf("=== Test: encrypt + HMAC m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 536;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)hmacKey;
  hmacCtx.keyLen = strlen(hmacKey);
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;
  cryptCtx.key = (const unsigned char *)cryptKey;
  cryptCtx.keyLen = strlen(cryptKey);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testEncryptDrop(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypted drop recovery test!";
  const char *key = "dropkey";

  printf("=== Test: encrypt + 20%% drop, multi-shard dgramMax=18 m=8 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8 */
  rsecCtx.tableSize = 64;
  cryptCtx.key = (const unsigned char *)key;
  cryptCtx.keyLen = strlen(key);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  m = mkBlob(&env.sa, 2, 1, 8, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

/* ===== HDR CALLBACK TESTS ===== */

static void
testHmacHdr(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacHdrCtx hmacCtx;
  chanBlb_t *m;
  const char *msg = "hmac hdr key derivation";
  const char *key = "hdrhmackey12";

  printf("=== Test: HMAC with hdr-dependent key ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 540;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  hmacCtx.tagSize = 4;
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacHdrSignCb;
  rsecCtx.hmacVrfy = hmacHdrVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 4, 0x1234, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 4, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testHmacHdrMultiTag(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacHdrCtx hmacCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[3];
  const char *key = "multitaghdr";
  const char *msgs[3];

  msgs[0] = "hdr tag msg zero";
  msgs[1] = "hdr tag msg one!";
  msgs[2] = "hdr tag msg two!";

  printf("=== Test: HMAC hdr multi-tag (different derived keys) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 540;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  hmacCtx.tagSize = 4;
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacHdrSignCb;
  rsecCtx.hmacVrfy = hmacHdrVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 4, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 2000000000L);
    if (!m) continue;
    for (j = 0; j < 3; ++j) {
      if (!seen[j] && verifyBlob(m, 4, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 3 hdr-derived key messages correct", ok == 3);
  teardown(&env, &rsecCtx);
}

static void
testEncryptHdr(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct cryptHdrCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypt hdr key derivation";
  const char *key = "hdrcryptkey1";

  printf("=== Test: encrypt with hdr-dependent key ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;
  cryptCtx.key = (const unsigned char *)key;
  cryptCtx.keyLen = strlen(key);
  cryptCtx.tagSize = 4;
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorHdrCryptCb;
  rsecCtx.decrypt = xorHdrCryptCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 4, 0x1234, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 4, msg, strlen(msg))); free(m); }
  teardown(&env, &rsecCtx);
}

static void
testEncryptHmacHdrDrop(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacHdrCtx hmacCtx;
  struct cryptHdrCtx cryptCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int j;
  unsigned int ok;
  int seen[3];
  const char *hmacKey = "hmacHdrDrop!";
  const char *cryptKey = "cryptHdrDrop";
  const char *msgs[3];

  msgs[0] = "hdr drop test msg zero!";
  msgs[1] = "hdr drop test msg one!!";
  msgs[2] = "hdr drop test msg two!!";

  printf("=== Test: encrypt+HMAC hdr + 40%% drop dgramMax=44 m=13 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 44;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)hmacKey;
  hmacCtx.keyLen = strlen(hmacKey);
  hmacCtx.tagSize = 4;
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacHdrSignCb;
  rsecCtx.hmacVrfy = hmacHdrVrfyCb;
  cryptCtx.key = (const unsigned char *)cryptKey;
  cryptCtx.keyLen = strlen(cryptKey);
  cryptCtx.tagSize = 4;
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorHdrCryptCb;
  rsecCtx.decrypt = xorHdrCryptCb;

  chanBlbTrnFdDatagramDropPct = 40;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 4, (unsigned short)(i + 1), 13, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto hdrDropDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 5000000000L);
    if (!m) continue;
    for (j = 0; j < 3; ++j) {
      if (!seen[j] && verifyBlob(m, 4, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 3 hdr-keyed messages delivered despite drops", ok == 3);
  rsecSnap(&rsecCtx);
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);

hdrDropDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env, &rsecCtx);
}

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

  printf("=== Test: egress interleave tableSize=2 delayMs=10 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 2;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 10, msgs[0], strlen(msgs[0]));
  check("send1", sendBlob(env.outChan, m));
  m = mkBlob(&env.sa, 2, 2, 1, 10, msgs[1], strlen(msgs[1]));
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

  printf("=== Test: egress backpressure tableSize=1 delayMs=50 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 1;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob1: k=1 m=1 km=2, delayMs=50 => paced over 50ms */
  m = mkBlob(&env.sa, 2, 1, 1, 50, msg1, strlen(msg1));
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

  printf("=== Test: backpressure counter tableSize=1 multi-shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 1;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob1: 27 bytes / 6 = 5 data, m=2, km=7, delayMs=10 */
  m = mkBlob(&env.sa, 2, 1, 2, 10, msg1, strlen(msg1));
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
  /* blob1: 7 frags, blob2: 1 frag */
  check("egrFrg == 8", rsecCtx.egrFrg == 8);
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
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8, k=4 for ~29 byte msgs */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=4 m=20 => 24 shards, need 4. 50% drop => P(loss) < 0.02% per msg */
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

  printf("=== Test: m clamping k=250 m=10->6 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8 */
  rsecCtx.tableSize = 64;

  /* k = ceil(2000/8) = 250, m=10 requested => km=260>256 => m clamped to 6 */
  payloadLen = 2000;
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
  check("egrFrg == 256", rsecCtx.egrFrg == 256);
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

  /* blob too short: only 2 bytes, needs at least prefixSize+2 */
  m = (chanBlb_t *)malloc(chanBlb_tSize(2));
  if (!m) { check("malloc", 0); teardown(&env, &rsecCtx); return; }
  m->l = 2;
  m->b[0] = 1;
  m->b[1] = 0;
  check("send malformed", sendBlob(env.outChan, m));

  /* egress should have exited on protocol violation; no message on ingress */
  m = recvBlob(env.inChan, 1000000000L);
  check("no message after protocol violation", m == 0);
  if (m) free(m);

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

  printf("=== Test: large k=250 m=1 (251 fragments) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 18; /* overhead=10, shardSize=8 */
  rsecCtx.tableSize = 64;

  payloadLen = 2000; /* k = ceil(2000/8) = 250, m=1 => 251 fragments */
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
  check("egrFrg == 251", rsecCtx.egrFrg == 251);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);

  free(payload);
  teardown(&env, &rsecCtx);
}

static void
testEncryptRsDecode(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hmacCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int j;
  unsigned int ok;
  int seen[5];
  const char *hmacKey = "hmacRsDecode";
  const char *cryptKey = "cryptRsDecode";
  const char *msgs[5];

  msgs[0] = "encrypt rs decode msg zero!";
  msgs[1] = "encrypt rs decode msg one!!";
  msgs[2] = "encrypt rs decode msg two!!";
  msgs[3] = "encrypt rs decode msg 3!!!";
  msgs[4] = "encrypt rs decode msg 4!!!";

  printf("=== Test: encrypt+HMAC+RS decode (50%% drop dgramMax=40 m=20) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 40; /* room for hmac */
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)hmacKey;
  hmacCtx.keyLen = strlen(hmacKey);
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;
  cryptCtx.key = (const unsigned char *)cryptKey;
  cryptCtx.keyLen = strlen(cryptKey);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  chanBlbTrnFdDatagramDropPct = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=2 m=20 km=22, 50% drop, 5 msgs: P(any fail)<0.003%, P(no decode)<0.1% */
  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 20, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto encRsDone; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 5; ++i) {
    m = recvBlob(env.inChan, 5000000000L);
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
  check("all 5 encrypted+HMAC messages delivered", ok == 5);
  rsecSnap(&rsecCtx);
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);

encRsDone:
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

static void
testPackHdr(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacHdrCtx hmacCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int ok;
  int seen[3];
  const char *key = "packhdrkey!!";
  const char *msgs[3];

  msgs[0] = "pack hdr msg zero";
  msgs[1] = "pack hdr msg one!";
  msgs[2] = "pack hdr msg two!";

  printf("=== Test: packing with hdr-dependent HMAC callbacks ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 4;
  rsecCtx.dgramMax = 508;
  rsecCtx.tableSize = 64;
  hmacCtx.key = (const unsigned char *)key;
  hmacCtx.keyLen = strlen(key);
  hmacCtx.tagSize = 4;
  rsecCtx.hmacSize = RMD128_SZ;
  rsecCtx.hmacCtx = &hmacCtx;
  rsecCtx.hmacSign = hmacHdrSignCb;
  rsecCtx.hmacVrfy = hmacHdrVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 4, (unsigned short)(i + 1), 1, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env, &rsecCtx); return; }
  }
  ok = 0;
  memset(seen, 0, sizeof (seen));
  for (i = 0; i < 3; ++i) {
    unsigned int j;

    m = recvBlob(env.inChan, 3000000000L);
    if (!m) continue;
    for (j = 0; j < 3; ++j) {
      if (!seen[j] && verifyBlob(m, 4, msgs[j], strlen(msgs[j]))) {
        seen[j] = 1;
        ++ok;
        break;
      }
    }
    free(m);
  }
  check("all 3 packed hdr-keyed messages correct", ok == 3);
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
 * Verify that egress packet emission honors the inter-shard delay:
 *   - Same-message shards never pack together (one packet each).
 *   - After emitting a packet, the next emit waits at least the
 *     largest delayMs among shards actually packed into that packet.
 * The observation hook in chanBlbTrnFdDatagramStress.c records
 * caller-side send timestamps into chanBlbTrnFdDatagramObs[]. */

static void
testPaceSingleMessage(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  const char *msg = "paced single";
  unsigned char delayMs;
  unsigned int i;
  unsigned int ok;
  long tol;

  /* k=1 m=4 => 5 shards, all from one message.  Same-message shards
   * never pack together, so each shard travels in its own datagram
   * and consecutive packets must be >= delayMs apart. */
  delayMs = 30;
  tol = 3; /* ms tolerance for clock/scheduling jitter */

  printf("=== Test: pace single message k=1 m=4 delayMs=30 ===\n");
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

  m = mkBlob(&env.sa, 2, 1, 4, delayMs, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  /* wait for all 5 shards to be emitted (total window ~ 5*delayMs) */
  usleep(300000);
  chanBlbTrnFdDatagramObsEnable = 0;

  rsecSnap(&rsecCtx);
  check("egrFrg == 5", rsecCtx.egrFrg == 5);
  check("5 datagrams emitted", chanBlbTrnFdDatagramObsCnt == 5);
  ok = 1;
  for (i = 1; i < chanBlbTrnFdDatagramObsCnt; ++i) {
    long g;

    g = obsDeltaMs(i - 1, i);
    if (g < (long)delayMs - tol) {
      printf("    gap[%u]=%ldms < %dms\n", i, g, delayMs);
      ok = 0;
    }
  }
  check("all inter-packet gaps >= delayMs", ok);

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
   * within a narrow window — packets emitted after the first should
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
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 0, 10,
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
  unsigned char delayA;
  unsigned char delayB;
  int seenA;
  int seenB;
  unsigned int i;
  unsigned int hasSlowGap;
  unsigned int sawFastGap;

  /* Construct a scenario that exercises "max delayMs in packed packet":
   *   msg A: k=1 m=1 (2 shards) delayMs=80  — scheduled at +80, +160
   *   msg B: k=1 m=3 (4 shards) delayMs=20  — scheduled at +20,+40,+60,+80
   * Both to the same address, queued back-to-back.  Expected emissions
   * (approximate, dropping insertion jitter):
   *   P1 @20: B0                       → lastMaxD=20
   *   P2 @40: B1                       → lastMaxD=20
   *   P3 @60: B2                       → lastMaxD=20
   *   P4 @80: A0 + B3 (packed, both due) → lastMaxD=80
   *   P5 @160 (= P4 + 80): A1          → lastMaxD=80
   * The critical check is gap(P5,P4) >= 80ms: a bug that used min
   * rather than max would schedule P5 only 20ms after P4. */
  delayA = 80;
  delayB = 20;

  printf("=== Test: pace respects max delayMs in packed packet ===\n");
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

  usleep(400000); /* wait for all shards to be emitted (~160ms + margin) */
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

    g = obsDeltaMs(i - 1, i);
    /* every gap must at minimum honor the fast delay */
    if (g >= (long)delayB - 3)
      ++sawFastGap;
    /* at least one gap must honor the slow delay */
    if (g >= (long)delayA - 3)
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
  unsigned char delayMs;

  msgs[0] = "no blast alpha";
  msgs[1] = "no blast bravo";
  msgs[2] = "no blast charlie";

  /* Three k=1 m=2 messages (3 shards each = 9 shards total) paced at
   * delayMs=25.  Maximal packing finishes in 3 datagrams (shards from
   * all 3 messages coalesced per tick); no packing emits 9.  The
   * bound we verify regardless of packing: no inter-packet gap may
   * be smaller than delayMs.  If the pacing logic were bypassed,
   * the egress would blast shards back-to-back. */
  delayMs = 25;

  printf("=== Test: pace 3 messages never blast (delayMs=25) ===\n");
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
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 2, delayMs,
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

    g = obsDeltaMs(i - 1, i);
    if (g < (long)delayMs - 3) {
      printf("    gap[%u]=%ldms < %dms\n", i, g, delayMs);
      ok = 0;
    }
  }
  check("no gap faster than delayMs", ok);

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

/* ===== rm/um + raw-injection coverage ===== */

/*
 * Craft and inject a single-fragment RSEC datagram directly into the
 * in-process loopback transport's address bus.  Uses a synthetic src
 * sockaddr_in so the delivered blob's addr prefix is well-formed; the
 * src content is irrelevant to these tests (identity lives in the tag).
 * Only 1-byte VLQs are emitted (padding < 128, shardLen < 128).
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

static int
injectFrag(
  int injFd
 ,const struct sockaddr_in *dst
 ,const unsigned char *tagBytes
 ,unsigned int tagSize
 ,unsigned char kMinus1
 ,unsigned char mVal
 ,unsigned char si
 ,unsigned char padding
 ,const unsigned char *shardData
 ,unsigned char shardLen
 ,const unsigned char *hmacBytes
 ,unsigned int hmacLen
 ,int corruptSmallHash
){
  unsigned char dg[512];
  unsigned int pos;
  unsigned int i;
  unsigned char h;
  struct sockaddr_in src;

  (void)injFd;
  pos = 0;
  memcpy(dg + pos, tagBytes, tagSize);
  pos += tagSize;
  dg[pos++] = kMinus1;
  dg[pos++] = mVal;
  dg[pos++] = si;
  dg[pos++] = padding;
  dg[pos++] = shardLen;
  memcpy(dg + pos, shardData, shardLen);
  pos += shardLen;
  if (hmacLen > 0) {
    memcpy(dg + pos, hmacBytes, hmacLen);
    pos += hmacLen;
  }
  h = 0xff;
  for (i = 0; i < pos; ++i) {
    h ^= dg[i];
    h = (unsigned char)((h << 1) | (h >> 7));
  }
  dg[pos++] = corruptSmallHash ? (unsigned char)(h ^ 0xff) : h;

  memset(&src, 0, sizeof (src));
  src.sin_family = AF_INET;
  src.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  src.sin_port = htons(1);  /* arbitrary non-zero marker */
  return (chanBlbTrnFdDatagramInject(
    (const struct sockaddr *)dst, (socklen_t)sizeof (*dst),
    (const struct sockaddr *)&src, (socklen_t)sizeof (src),
    dg, pos));
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
  rsecCtx.dgramMax = 18;   /* shardSize=8 => k=4 for 27 bytes */
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
  chanBlb_t *m;
  int injFd;
  unsigned char tag[2];
  unsigned char shard[3];

  printf("=== Test: igrHash increments on bad smallhash ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0x11; tag[1] = 0x22;
  shard[0] = 'a'; shard[1] = 'b'; shard[2] = 'c';
  check("inject bad-smallhash frag",
    injectFrag(injFd, &env.sa, tag, 2, 0, 0, 0, 0, shard, 3, 0, 0, 1));

  m = recvBlob(env.inChan, 500000000L);
  check("no delivery on bad smallhash", m == 0);
  if (m) free(m);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrHash >= 1", rsecCtx.igrHash >= 1);
  check("igrMsg == 0", rsecCtx.igrMsg == 0);

  close(injFd);
  teardown(&env, &rsecCtx);
}

static void
testIgrHmacReject(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  struct hmacKeyCtx hk;
  static const unsigned char key[] = "hmac-key-for-reject-test";
  unsigned char badHmac[16];
  unsigned char tag[2];
  unsigned char shard[3];
  chanBlb_t *m;
  int injFd;
  unsigned int i;

  printf("=== Test: igrHmac increments on bad HMAC ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;
  rsecCtx.hmacSize = 16;
  hk.key = key;
  hk.keyLen = sizeof (key) - 1;
  rsecCtx.hmacCtx = &hk;
  rsecCtx.hmacSign = hmacSignCb;
  rsecCtx.hmacVrfy = hmacVrfyCb;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0x33; tag[1] = 0x44;
  shard[0] = 'x'; shard[1] = 'y'; shard[2] = 'z';
  for (i = 0; i < sizeof (badHmac); ++i)
    badHmac[i] = (unsigned char)(i * 13 + 5);
  check("inject bad-HMAC frag (valid smallhash)",
    injectFrag(injFd, &env.sa, tag, 2, 0, 0, 0, 0, shard, 3,
               badHmac, sizeof (badHmac), 0));

  m = recvBlob(env.inChan, 500000000L);
  check("no delivery on bad HMAC", m == 0);
  if (m) free(m);

  usleep(100000);
  rsecSnap(&rsecCtx);
  check("igrHmac >= 1", rsecCtx.igrHmac >= 1);
  check("igrHash == 0 (smallhash was valid)", rsecCtx.igrHash == 0);
  check("igrMsg == 0", rsecCtx.igrMsg == 0);

  close(injFd);
  teardown(&env, &rsecCtx);
}

static void
testIgrDupReject(void)
{
  struct testEnv env;
  struct rsecPair rsecCtx;
  chanBlb_t *m;
  int injFd;
  unsigned char tag[2];
  unsigned char s0[3];
  unsigned char s1[3];
  unsigned int al;
  unsigned int off;

  printf("=== Test: igrDup increments on pre-delivery duplicate shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0x55; tag[1] = 0x66;
  s0[0] = 'A'; s0[1] = 'B'; s0[2] = 'C';
  s1[0] = 'D'; s1[1] = 'E'; s1[2] = 'F';
  /* k=2 (kMinus1=1) m=0: need both shards. Inject si=0 twice, then si=1. */
  check("inject shard 0",
    injectFrag(injFd, &env.sa, tag, 2, 1, 0, 0, 0, s0, 3, 0, 0, 0));
  usleep(50000);
  check("inject shard 0 duplicate",
    injectFrag(injFd, &env.sa, tag, 2, 1, 0, 0, 0, s0, 3, 0, 0, 0));
  usleep(50000);
  check("inject shard 1",
    injectFrag(injFd, &env.sa, tag, 2, 1, 0, 1, 0, s1, 3, 0, 0, 0));

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

  close(injFd);
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
  int injFd;
  unsigned char tag[2];
  unsigned char s0a[3];
  unsigned char s0b[4];
  unsigned int al;
  unsigned int off;
  unsigned int i;

  printf("=== Test: parameter-mismatch eviction delivers loss notification ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0x77; tag[1] = 0x88;
  s0a[0] = 'P'; s0a[1] = 'Q'; s0a[2] = 'R';
  s0b[0] = 'X'; s0b[1] = 'Y'; s0b[2] = 'Z'; s0b[3] = '!';

  /* msg1: k=2 (kMinus1=1), m=0, shardSize=3.  Inject only shard 0 -> incomplete. */
  check("inject msg1 shard 0 (incomplete)",
    injectFrag(injFd, &env.sa, tag, 2, 1, 0, 0, 0, s0a, 3, 0, 0, 0));
  usleep(80000);

  /* msg2: same tag, k=1 (kMinus1=0), m=0, shardSize=4.  Params mismatch. */
  check("inject msg2 shard 0 (param mismatch)",
    injectFrag(injFd, &env.sa, tag, 2, 0, 0, 0, 0, s0b, 4, 0, 0, 0));

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

  close(injFd);
  teardown(&env, &rsecCtx);
}

/*
 * Same tag, same k, same shardSize — only m differs.
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
  int injFd;
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

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0x99; tag[1] = 0xAA;
  shard[0] = 'M'; shard[1] = 'N'; shard[2] = 'O';

  /* k=1 m=0 shardSize=3 -> completes on arrival */
  check("inject msg with m=0",
    injectFrag(injFd, &env.sa, tag, 2, 0, 0, 0, 0, shard, 3, 0, 0, 0));
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
    injectFrag(injFd, &env.sa, tag, 2, 0, 5, 0, 0, shard, 3, 0, 0, 0));
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

  close(injFd);
  teardown(&env, &rsecCtx);
}

/*
 * Same tag, same k, same shardSize — only m differs, but the original
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
  int injFd;
  unsigned char tag[2];
  unsigned char s0[3];
  unsigned int al;
  unsigned int off;
  unsigned int i;

  printf("=== Test: same tag, m differs, original incomplete -> loss notification ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 8;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  injFd = socket(AF_INET, SOCK_DGRAM, 0);
  check("injector socket", injFd >= 0);

  tag[0] = 0xBB; tag[1] = 0xCC;
  s0[0] = 'P'; s0[1] = 'Q'; s0[2] = 'R';

  /* k=2 m=3 shardSize=3 -> incomplete after 1 shard */
  check("inject shard 0 (k=2 m=3, incomplete)",
    injectFrag(injFd, &env.sa, tag, 2, 1, 3, 0, 0, s0, 3, 0, 0, 0));
  usleep(50000);

  /* Same tag, same k, same shardSize, different m (7).  m-only mismatch
   * still triggers param-mismatch eviction; the original is incomplete
   * (blob != NULL) so igrEvict++ and a loss notification is delivered. */
  check("inject shard 0 with different m (7)",
    injectFrag(injFd, &env.sa, tag, 2, 1, 7, 0, 0, s0, 3, 0, 0, 0));

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

  close(injFd);
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
  testMultiShard();
  testEmptyPayload();
  testSequential();
  testHmacMatch();
  testHmacMultiShard();
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
  testStressMultiShardHmac();

  /* counter tests */
  testCountersBasic();
  testCountersMultiShard();
  testCountersDrop();
  testCountersDedup();

  /* shard size API */
  testShardApi();

  /* egress pacing tests */
  testEgressInterleave();
  testEgressBackpressure();
  testCountersBackpressure();

  /* encrypt/decrypt tests */
  testEncryptBasic();
  testEncryptMultiShard();
  testEncryptHmac();
  testEncryptDrop();

  /* hdr callback tests */
  testHmacHdr();
  testHmacHdrMultiTag();
  testEncryptHdr();
  testEncryptHmacHdrDrop();

  /* coverage gap tests */
  testRsDecodeAliasing();
  testMClampIntermediate();
  testMalformedBlob();
  testEmptyPayloadDrop();
  testTagSizeZero();
  testLargeK();
  testEncryptRsDecode();

  /* packing tests */
  testPackSmall();
  testPackMixedSize();
  testPackDrop();
  testPackHdr();
  testPackCounters();
  testPackSameMessage();

  /* pacing tests */
  testPaceSingleMessage();
  testPacePackingDensity();
  testPaceMaxDelayMixed();
  testPaceNoBlasting();

  /* rm/um + raw-injection coverage */
  testRmUmNoDecode();
  testRmUmWithDecode();
  testIgrHashReject();
  testIgrHmacReject();
  testIgrDupReject();
  testParamMismatchLossNotif();
  testSameTagMOnlyDelivered();
  testSameTagMOnlyIncomplete();

  printf("\n=======================================\n");
  printf("Results: %d passed, %d failed\n", Pass, Fail);
  printf("=======================================\n");
  return (Fail > 0 ? 1 : 0);
}
