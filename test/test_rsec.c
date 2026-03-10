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
 * [addrlen(1)][addr(addrlen)][tag(tagSize)][m(1)][payload]
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
  skip = 1 + al + tagSize + 1;
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

static int
setup(
  struct testEnv *env
 ,struct chanBlbChnRsecCtx *rsecCtx
){
  void *dgramCtx;
  socklen_t sl;
  int one;
  int bufsz;

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

  dgramCtx = chanBlbTrnFdDatagramCtx();
  if (!dgramCtx) {
    close(env->fd);
    return (0);
  }

  env->outChan = chanCreate(free, 0);
  env->inChan = chanCreate(free, 0);
  if (!env->outChan || !env->inChan) {
    close(env->fd);
    free(dgramCtx);
    return (0);
  }

  if (!chanBlb(realloc, free
      ,env->outChan, chanBlbTrnFdDatagramOutputCtx(dgramCtx, env->fd, -1), chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, rsecCtx, chanBlbChnRsecEgr
      ,env->inChan, chanBlbTrnFdDatagramInputCtx(dgramCtx, env->fd, -1), chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, rsecCtx, chanBlbChnRsecIgr, 0
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
){
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
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testBasicM0(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testDedupM2(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard message requiring several fragments";

  printf("=== Test: multi-shard (dgramMax=16) ===\n");
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
  teardown(&env);
}

static void
testEmptyPayload(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testSequential(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
  }
  ok = 0;
  for (i = 0; i < 5; ++i) {
    m = recvBlob(env.inChan, 2000000000L);
    if (m && verifyBlob(m, 2, msgs[i], strlen(msgs[i])))
      ++ok;
    free(m);
  }
  check("all 5 messages correct", ok == 5);
  teardown(&env);
}

static void
testHmacMatch(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testHmacMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testTinyShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "stress test with tiny shards yep";

  printf("=== Test: tiny dgramMax=12 m=2 (many fragments) ===\n");
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
  teardown(&env);
}

static void
testVlq2Byte(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "x";

  printf("=== Test: 2-byte VLQ padding (dgramMax=264, 1-byte payload) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 264;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, 1);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 2000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, 1)); free(m); }
  teardown(&env);
}

static void
testVlqMaxShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "x";

  printf("=== Test: VLQ max shard (dgramMax=16392, shardSize=16384) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16392; /* overhead=8, shardSize=16384, padding=16383 */
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 1-byte payload, shardSize=16384 => padding=16383 (max 2-byte VLQ) */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, 1);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, 1)); free(m); }
  teardown(&env);
}

static void
testDgramMaxValidation(void)
{
  struct chanBlbChnRsecCtx rsecCtx;

  printf("=== Test: dgramMax validation (VLQ limit) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;

  /* overhead=2+0+6=8, max shardSize=16384, max dgramMax=16392 */
  rsecCtx.dgramMax = 16392;
  check("16392 shard == 16384", chanBlbChnRsecShard(&rsecCtx) == 16384);
  check("16392 max > 0", chanBlbChnRsecMax(&rsecCtx, 1) > 0);

  rsecCtx.dgramMax = 16393;
  check("16393 shard == 0", chanBlbChnRsecShard(&rsecCtx) == 0);
  check("16393 max == 0", chanBlbChnRsecMax(&rsecCtx, 1) == 0);

  /* with HMAC: overhead=2+16+6=24, max dgramMax=16408 */
  rsecCtx.hmacSize = 16;
  rsecCtx.dgramMax = 16408;
  check("hmac 16408 shard == 16384", chanBlbChnRsecShard(&rsecCtx) == 16384);

  rsecCtx.dgramMax = 16409;
  check("hmac 16409 shard == 0", chanBlbChnRsecShard(&rsecCtx) == 0);
}

static void
testHighParity(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

/* ===== MAX PAYLOAD TESTS ===== */

static void
testMaxApi(void)
{
  struct chanBlbChnRsecCtx rsecCtx;

  printf("=== Test: chanBlbChnRsecMax API ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520; /* overhead=2+0+6=8, shardSize=512 */

  check("m=0 => 256*512=131072", chanBlbChnRsecMax(&rsecCtx, 0) == 131072);
  check("m=1 => 255*512=130560", chanBlbChnRsecMax(&rsecCtx, 1) == 130560);
  check("m=5 => 251*512=128512", chanBlbChnRsecMax(&rsecCtx, 5) == 128512);
  check("m=255 => 1*512=512", chanBlbChnRsecMax(&rsecCtx, 255) == 512);

  rsecCtx.dgramMax = 16; /* overhead=8, shardSize=8 */
  check("d=16 m=2 => 254*8=2032", chanBlbChnRsecMax(&rsecCtx, 2) == 2032);

  rsecCtx.dgramMax = 8; /* overhead=8, shardSize=0: too small */
  check("dgramMax==overhead => 0", chanBlbChnRsecMax(&rsecCtx, 0) == 0);
}

static void
testMaxBoundary(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  unsigned int maxLen;
  unsigned char *payload;

  printf("=== Test: max payload boundary (dgramMax=16 m=1) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  maxLen = chanBlbChnRsecMax(&rsecCtx, 1); /* 255 * 8 = 2040 */
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
  teardown(&env);
}

/* ===== DROP TESTS ===== */

static void
testDropRecovery(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop recovery test message";

  printf("=== Test: 30%% drop, m=4 single shard ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1, m=4 => 5 shards. Need any 1 of 5. P(all 5 dropped) = 0.3^5 = 0.24% */
  m = mkBlob(&env.sa, 2, 1, 4, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

static void
testDropMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop multi shard recovery test!";

  printf("=== Test: 20%% drop, multi-shard dgramMax=16 m=4 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* 31 bytes / 8 = 4 data shards, m=4 => 8 total. Need any 4 of 8. */
  m = mkBlob(&env.sa, 2, 1, 4, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

static void
testDropMultipleMessages(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  printf("=== Test: 25%% drop, 10 sequential messages, m=5 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 25;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1, m=5 => 6 shards each. P(all 6 dropped) = 0.25^6 < 0.025% per msg */
  for (i = 0; i < 10; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 5, 0,
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
  teardown(&env);
}

/* ===== DELAY (REORDER) TESTS ===== */

static void
testDelayReorder(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "delayed and reordered fragments";

  printf("=== Test: 50ms delay (reorder), multi-shard dgramMax=16 m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDelayMs = 50;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDelayMs = 0; return; }

  /* 31 bytes / 8 = 4 data + 1 parity = 5 fragments, randomly delayed */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received after delay", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDelayMs = 0;
  teardown(&env);
}

static void
testDelaySequential(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

/* ===== COMBINED STRESS TESTS ===== */

static void
testStressCombined(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
stressDone:
  chanBlbTrnFdDatagramDropPct = 0;
  chanBlbTrnFdDatagramDelayMs = 0;
}

static void
testStressMultiShardHmac(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  printf("=== Test: 15%% drop + 30ms delay, multi-shard dgramMax=40 m=3, HMAC ===\n");
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

  chanBlbTrnFdDatagramDropPct = 15;
  chanBlbTrnFdDatagramDelayMs = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); goto stressHmacDone; }

  for (i = 0; i < 5; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 3, 0,
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
  teardown(&env);
stressHmacDone:
  chanBlbTrnFdDatagramDropPct = 0;
  chanBlbTrnFdDatagramDelayMs = 0;
}

/* ===== COUNTER TESTS ===== */

static void
testCountersBasic(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 2", rsecCtx.egrFrg == 2);
  check("igrFrg == 2", rsecCtx.igrFrg == 2);
  check("igrHash == 0", rsecCtx.igrHash == 0);
  check("igrHmac == 0", rsecCtx.igrHmac == 0);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDcd == 0", rsecCtx.igrDcd == 0);
  check("igrDup == 0", rsecCtx.igrDup == 0);
  check("igrLate > 0", rsecCtx.igrLate > 0);
  check("igrLost == 0", rsecCtx.igrLost == 0);
  teardown(&env);
}

static void
testCountersMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "multi shard counter test msg!";

  printf("=== Test: counters multi-shard s=8 m=1 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* 29 bytes / 8 = 4 data shards, m=1 => 5 fragments */
  m = mkBlob(&env.sa, 2, 1, 1, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 5", rsecCtx.egrFrg == 5);
  check("igrFrg == 5", rsecCtx.igrFrg == 5);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDcd == 0", rsecCtx.igrDcd == 0);
  check("igrLost == 0", rsecCtx.igrLost == 0);
  teardown(&env);
}

static void
testCountersDrop(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  const char *msg = "drop counter test message!!";

  printf("=== Test: counters with 20%% drop, multi-shard s=8 m=5 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* 27 bytes / 8 = 4 data, m=5 => 9 fragments sent, need 4 */
  m = mkBlob(&env.sa, 2, 1, 5, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received", m != 0);
  if (m) free(m);
  usleep(100000);

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 9", rsecCtx.egrFrg == 9);
  check("igrFrg <= 9", rsecCtx.igrFrg <= 9);
  check("igrFrg >= 4", rsecCtx.igrFrg >= 4);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  /* if any data shard was dropped, RS decode was needed */
  printf("    (igrDcd=%u igrFrg=%u igrDup=%u)\n",
    rsecCtx.igrDcd, rsecCtx.igrFrg, rsecCtx.igrDup);
  check("igrLost == 0", rsecCtx.igrLost == 0);

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

static void
testCountersDedup(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 3", rsecCtx.egrFrg == 3);
  check("igrFrg == 3", rsecCtx.igrFrg == 3);
  check("igrMsg == 1", rsecCtx.igrMsg == 1);
  check("igrDup == 0", rsecCtx.igrDup == 0);
  check("igrLate == 2", rsecCtx.igrLate == 2);
  check("igrLost == 0", rsecCtx.igrLost == 0);
  teardown(&env);
}

/* ===== SHARD SIZE API TESTS ===== */

static void
testShardApi(void)
{
  struct chanBlbChnRsecCtx rsecCtx;

  printf("=== Test: chanBlbChnRsecShard API ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520; /* overhead=2+0+6=8, shardSize=512 */

  check("shard=512", chanBlbChnRsecShard(&rsecCtx) == 512);

  rsecCtx.hmacSize = 16; /* overhead=2+16+6=24, shardSize=496 */
  check("shard=496 with hmac", chanBlbChnRsecShard(&rsecCtx) == 496);

  rsecCtx.dgramMax = 16;
  rsecCtx.hmacSize = 0; /* overhead=2+0+6=8, shardSize=8 */
  check("shard=8 small dgram", chanBlbChnRsecShard(&rsecCtx) == 8);

  rsecCtx.dgramMax = 8; /* overhead=8, shardSize=0: too small */
  check("dgramMax==overhead => 0", chanBlbChnRsecShard(&rsecCtx) == 0);
}

/* ===== ENCRYPT/DECRYPT TESTS ===== */

static void
testEncryptBasic(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testEncryptMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testEncryptHmac(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testEncryptDrop(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  const char *msg = "encrypted drop recovery test!";
  const char *key = "dropkey";

  printf("=== Test: encrypt + 20%% drop, multi-shard dgramMax=16 m=4 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16;
  rsecCtx.tableSize = 64;
  cryptCtx.key = (const unsigned char *)key;
  cryptCtx.keyLen = strlen(key);
  rsecCtx.cryptCtx = &cryptCtx;
  rsecCtx.encrypt = xorCryptCb;
  rsecCtx.decrypt = xorCryptCb;

  chanBlbTrnFdDatagramDropPct = 20;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  m = mkBlob(&env.sa, 2, 1, 4, 0, msg, strlen(msg));
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 5000000000L);
  check("received after drops", m != 0);
  if (m) { check("payload correct", verifyBlob(m, 2, msg, strlen(msg))); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

/* ===== EGRESS PACING TESTS ===== */

static void
testEgressInterleave(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  teardown(&env);
}

static void
testEgressBackpressure(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  teardown(&env);
}

static void
testCountersBackpressure(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  /* blob1: 27 bytes / 8 = 4 data, m=2, km=6, delayMs=10 */
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
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  /* blob1: 6 frags, blob2: 1 frag */
  check("egrFrg == 7", rsecCtx.egrFrg == 7);
  teardown(&env);
}

/* ===== COVERAGE GAP TESTS ===== */

static void
testRsDecodeAliasing(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  rsecCtx.dgramMax = 16; /* shardSize=8, k=4 for ~29 byte msgs */
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=4 m=8 => 12 shards, need 4. 30% drop => P(loss) < 0.2% per msg */
  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 8, 0,
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
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);

rsAliasDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

static void
testMClampIntermediate(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  unsigned char *payload;
  unsigned int payloadLen;
  unsigned int i;

  printf("=== Test: m clamping k=250 m=10->6 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16; /* shardSize=8 */
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
  /* m clamped from 10 to 6: 250 data + 6 parity = 256 fragments */
  check("egrFrg == 256", rsecCtx.egrFrg == 256);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);

  free(payload);
  teardown(&env);
}

static void
testMalformedBlob(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;

  printf("=== Test: malformed blob protocol violation ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); return; }

  /* blob too short: only 2 bytes, needs at least prefixSize+2 */
  m = (chanBlb_t *)malloc(chanBlb_tSize(2));
  if (!m) { check("malloc", 0); teardown(&env); return; }
  m->l = 2;
  m->b[0] = 1;
  m->b[1] = 0;
  check("send malformed", sendBlob(env.outChan, m));

  /* egress should have exited on protocol violation; no message on ingress */
  m = recvBlob(env.inChan, 1000000000L);
  check("no message after protocol violation", m == 0);
  if (m) free(m);

  teardown(&env);
}

static void
testEmptyPayloadDrop(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;

  printf("=== Test: empty payload with 30%% drop, m=4 ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 520;
  rsecCtx.tableSize = 64;

  chanBlbTrnFdDatagramDropPct = 30;

  if (!setup(&env, &rsecCtx)) { check("setup", 0); chanBlbTrnFdDatagramDropPct = 0; return; }

  /* k=1 m=4 => 5 shards, need 1. P(all dropped) = 0.3^5 < 0.25% */
  m = mkBlob(&env.sa, 2, 1, 4, 0, 0, 0);
  check("send", sendBlob(env.outChan, m));
  m = recvBlob(env.inChan, 3000000000L);
  check("received", m != 0);
  if (m) { check("empty payload", verifyBlob(m, 2, 0, 0)); free(m); }

  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

static void
testTagSizeZero(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  teardown(&env);
}

static void
testLargeK(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  unsigned char *payload;
  unsigned int payloadLen;
  unsigned int i;

  printf("=== Test: large k=250 m=1 (251 fragments) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;
  rsecCtx.dgramMax = 16; /* shardSize=8 */
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
  check("egrFrg == 251", rsecCtx.egrFrg == 251);
  check("egrMsg == 1", rsecCtx.egrMsg == 1);

  free(payload);
  teardown(&env);
}

static void
testEncryptRsDecode(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  struct hmacKeyCtx hmacCtx;
  struct cryptKeyCtx cryptCtx;
  chanBlb_t *m;
  unsigned int i;
  unsigned int j;
  unsigned int ok;
  int seen[3];
  const char *hmacKey = "hmacRsDecode";
  const char *cryptKey = "cryptRsDecode";
  const char *msgs[3];

  msgs[0] = "encrypt rs decode msg zero!";
  msgs[1] = "encrypt rs decode msg one!!";
  msgs[2] = "encrypt rs decode msg two!!";

  printf("=== Test: encrypt+HMAC+RS decode (50%% drop dgramMax=40 m=8) ===\n");
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

  for (i = 0; i < 3; ++i) {
    m = mkBlob(&env.sa, 2, (unsigned short)(i + 1), 8, 0,
               msgs[i], strlen(msgs[i]));
    if (!sendBlob(env.outChan, m)) { check("send", 0); goto encRsDone; }
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
  check("all 3 encrypted+HMAC messages delivered", ok == 3);
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);

encRsDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
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

  /* coverage gap tests */
  testRsDecodeAliasing();
  testMClampIntermediate();
  testMalformedBlob();
  testEmptyPayloadDrop();
  testTagSizeZero();
  testLargeK();
  testEncryptRsDecode();

  printf("\n=======================================\n");
  printf("Results: %d passed, %d failed\n", Pass, Fail);
  printf("=======================================\n");
  return (Fail > 0 ? 1 : 0);
}
