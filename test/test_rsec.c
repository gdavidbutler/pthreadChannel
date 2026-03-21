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
      ,env->outChan, chanBlbTrnFdDatagramOutputCtx(dgramCtx, &env->fd, 0, 1, 0), chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, rsecCtx, chanBlbChnRsecEgr
      ,env->inChan, chanBlbTrnFdDatagramInputCtx(dgramCtx, &env->fd, 0, 1, 0), chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, rsecCtx, chanBlbChnRsecIgr, 0
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
  teardown(&env);
}

static void
testVlq2Byte(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testVlqMaxShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testDgramMaxValidation(void)
{
  struct chanBlbChnRsecCtx rsecCtx;

  printf("=== Test: dgramMax validation (VLQ limit) ===\n");
  memset(&rsecCtx, 0, sizeof (rsecCtx));
  rsecCtx.tagSize = 2;

  /* overhead=2+0+8=10, max shardSize=16384, max dgramMax=16394 */
  rsecCtx.dgramMax = 16394;
  check("16394 shard == 16384", chanBlbChnRsecShard(&rsecCtx) == 16384);
  check("16394 max > 0", chanBlbChnRsecMax(&rsecCtx, 1) > 0);

  rsecCtx.dgramMax = 16395;
  check("16395 shard == 0", chanBlbChnRsecShard(&rsecCtx) == 0);
  check("16395 max == 0", chanBlbChnRsecMax(&rsecCtx, 1) == 0);

  /* with HMAC: overhead=2+16+8=26, max dgramMax=16410 */
  rsecCtx.hmacSize = 16;
  rsecCtx.dgramMax = 16410;
  check("hmac 16410 shard == 16384", chanBlbChnRsecShard(&rsecCtx) == 16384);

  rsecCtx.dgramMax = 16411;
  check("hmac 16411 shard == 0", chanBlbChnRsecShard(&rsecCtx) == 0);
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
  rsecCtx.dgramMax = 520; /* overhead=2+0+8=10, shardSize=510 */

  check("m=0 => 256*510=130560", chanBlbChnRsecMax(&rsecCtx, 0) == 130560);
  check("m=1 => 255*510=130050", chanBlbChnRsecMax(&rsecCtx, 1) == 130050);
  check("m=5 => 251*510=128010", chanBlbChnRsecMax(&rsecCtx, 5) == 128010);
  check("m=255 => 1*510=510", chanBlbChnRsecMax(&rsecCtx, 255) == 510);

  rsecCtx.dgramMax = 16; /* overhead=10, shardSize=6 */
  check("d=16 m=2 => 254*6=1524", chanBlbChnRsecMax(&rsecCtx, 2) == 1524);

  rsecCtx.dgramMax = 10; /* overhead=10, shardSize=0: too small */
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

  maxLen = chanBlbChnRsecMax(&rsecCtx, 1); /* 255 * 6 = 1530 */
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
  teardown(&env);
}

static void
testDropMultiShard(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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

  /* 31 bytes / 6 = 6 data + 1 parity = 7 fragments, randomly delayed */
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

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 6", rsecCtx.egrFrg == 6);
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

  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  check("egrFrg == 12", rsecCtx.egrFrg == 12);
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
  rsecCtx.dgramMax = 520; /* overhead=2+0+8=10, shardSize=510 */

  check("shard=510", chanBlbChnRsecShard(&rsecCtx) == 510);

  rsecCtx.hmacSize = 16; /* overhead=2+16+8=26, shardSize=494 */
  check("shard=494 with hmac", chanBlbChnRsecShard(&rsecCtx) == 494);

  rsecCtx.dgramMax = 16;
  rsecCtx.hmacSize = 0; /* overhead=2+0+8=10, shardSize=6 */
  check("shard=6 small dgram", chanBlbChnRsecShard(&rsecCtx) == 6);

  rsecCtx.dgramMax = 10; /* overhead=10, shardSize=0: too small */
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
  teardown(&env);
}

/* ===== HDR CALLBACK TESTS ===== */

static void
testHmacHdr(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testHmacHdrMultiTag(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
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
  teardown(&env);
}

static void
testEncryptHdr(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testEncryptHmacHdrDrop(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);

hdrDropDone:
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
  check("egrMsg == 2", rsecCtx.egrMsg == 2);
  /* blob1: 7 frags, blob2: 1 frag */
  check("egrFrg == 8", rsecCtx.egrFrg == 8);
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
  printf("    (igrDcd=%u)\n", rsecCtx.igrDcd);
  check("RS decode was used (igrDcd > 0)", rsecCtx.igrDcd > 0);

encRsDone:
  chanBlbTrnFdDatagramDropPct = 0;
  teardown(&env);
}

/* ===== PACKING TESTS ===== */

static void
testPackSmall(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
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
  teardown(&env);
}

static void
testPackMixedSize(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
  chanBlb_t *m;
  unsigned int ok;
  int seen[3];
  unsigned int i;
  const char *small1 = "tiny";
  const char *small2 = "also tiny";
  unsigned char big[400];
  const char *msgs[2];

  msgs[0] = small1;
  msgs[1] = small2;

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
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
  m = mkBlob(&env.sa, 2, 2, 1, 0, small2, strlen(small2));
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
  m = mkBlob(&env.sa, 2, 3, 1, 0, big, sizeof (big) / sizeof (big[0]));
  if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }

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
  teardown(&env);
}

static void
testPackDrop(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  teardown(&env);
}

static void
testPackHdr(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
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
  teardown(&env);
}

static void
testPackCounters(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
    if (!sendBlob(env.outChan, m)) { check("send", 0); teardown(&env); return; }
  }
  ok = 0;
  for (i = 0; i < 3; ++i) {
    m = recvBlob(env.inChan, 3000000000L);
    if (m) { ++ok; free(m); }
  }
  check("all 3 received", ok == 3);
  usleep(100000);

  check("egrMsg == 3", rsecCtx.egrMsg == 3);
  check("egrFrg == 6", rsecCtx.egrFrg == 6);
  check("igrMsg == 3", rsecCtx.igrMsg == 3);
  teardown(&env);
}

static void
testPackSameMessage(void)
{
  struct testEnv env;
  struct chanBlbChnRsecCtx rsecCtx;
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
  check("egrMsg == 1", rsecCtx.egrMsg == 1);
  /* 4 fragments from one message: none should be packed together */
  check("egrFrg == 4", rsecCtx.egrFrg == 4);
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

  printf("\n=======================================\n");
  printf("Results: %d passed, %d failed\n", Pass, Fail);
  printf("=======================================\n");
  return (Fail > 0 ? 1 : 0);
}
