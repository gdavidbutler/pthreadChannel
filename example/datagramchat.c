/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2025 G. David Butler <gdb@dbSystems.com>
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

/* Generated with Claude Code (https://claude.ai/code) */

/* Broadcast datagram chat demonstrating chanBlbTrnFdDatagram */
/* Compile with -DRSEC to enable Reed-Solomon erasure coding for reliability */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "chanBlbTrnFdDatagram.h"
#ifdef RSEC
#include <time.h>
#include "chanBlbChnRsec.h"
#include "rmd128.h"
#endif

struct peer {
  struct sockaddr_storage addr;
  socklen_t len;
};

static struct peer *Peers;
static int Npeers;
static chan_t *OutChan;
static chan_t *InChan;
#ifdef RSEC
/* a tag is [this run's 4 bytes, of its start time and pid][a line
 * counter(2)]: it names the sender, so two peers' lines do not share
 * one at a third */
static unsigned int TagSize = 6;
static unsigned char Sender[4];
static unsigned int DgramMax = 508;
static unsigned int TableSize = 64;
static unsigned int ParityShards = 1;
static unsigned long DelayUs = 100;
static unsigned short TagCounter;
static char *HmacKey;

/* -k: an RMD128-HMAC a datagram as chanBlbChnRsec's GATE, every peer
 * holding the one key */
struct hmacKeyCtx {
  const unsigned char *key;
  unsigned int keyLen;
};

static void
gateSignCb(
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
gateVrfyCb(
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
#endif

/*
 * The egress's close, when its thread has ended: everything put on the
 * egress channel has been sent, so the ingress channel is shut, the
 * display thread ends, and main's join of it returns.
 */
static void
outClose(
  void *v
){
  chanBlbTrnFdDatagramOutputClose(v);
  chanShut(InChan);
}

/* display thread: read from ingress channel, print [source]: message */
static void *
displayT(
  void *v
){
  chanBlb_t *m;
  chanArr_t p[1];

  (void)v;
  p[0].c = InChan;
  p[0].v = (void **)&m;
  p[0].o = chanOpGet;
  while (chanOne(0, sizeof (p) / sizeof (p[0]), p) == 1 && p[0].s == chanOsGet) {
    unsigned int al;
    unsigned int ml;
    unsigned int skip;
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];

    al = m->b[0];
#ifdef RSEC
    /* ingress blob: [addrlen(1)][addr(al)][tag(TagSize)][rm(1)][um(1)][payload] */
    skip = TagSize + 2;
#else
    skip = 0;
#endif
    ml = m->l - 1 - al - skip;
    if (1 + al + skip <= m->l
     && !getnameinfo((struct sockaddr *)(m->b + 1), al
         ,host, sizeof (host), serv, sizeof (serv)
         ,NI_NUMERICHOST | NI_NUMERICSERV)) {
#ifdef RSEC
      /* a loss notification: rm 0, um the fragments the lost one had */
      if (ml == 0 && !m->b[1 + al + TagSize] && m->b[1 + al + TagSize + 1])
        printf("[%s:%s]: (a message lost)\n", host, serv);
      else
#endif
      if (ml == 0)
        printf("[%s:%s]: left\n", host, serv);
      else
        printf("[%s:%s]: %.*s", host, serv, ml, m->b + 1 + al + skip);
      fflush(stdout);
    }
    free(m);
  }
  return (0);
}

static char const *ProgName;

static int
usage(
  void
){
  fprintf(stderr
  ,"Usage: %s -l port"
#ifdef RSEC
   " [-m parity] [-f dgrammax] [-d delayus] [-t tablesize] [-k key]"
#endif
   " peer:port [peer:port ...]\n"
   "  Broadcast chat using unbound datagram sockets\n"
#ifdef RSEC
   "  -m  parity shards (default 1)\n"
   "  -f  max datagram payload size (default 508, IPv4/UDP fragment-free)\n"
   "  -d  inter-shard delay (microseconds, default 100)\n"
   "  -t  table size, each direction (default 64)\n"
   "  -k  shared key: an rmd128 HMAC on every datagram, a datagram\n"
   "      without it dropped\n"
#endif
  ,ProgName
  );
  return (1);
}

int
main(
  int argc
 ,char *argv[]
){
  struct addrinfo hint;
  struct addrinfo *addr;
  char *lport;
  int i;
  int fd4, fd6;
  void *ctx;
  void *oc;
  void *ic;
  pthread_t dt;

  ProgName = argv[0];
  lport = 0;

  while ((i = getopt(argc, argv,
#ifdef RSEC
    "l:m:f:d:t:k:"
#else
    "l:"
#endif
  )) != -1) switch (i) {
  case 'l':
    lport = optarg;
    break;
#ifdef RSEC
  case 'm':
    if ((ParityShards = atoi(optarg)) > 255)
      return (usage());
    break;
  case 'f':
    DgramMax = (unsigned int)atoi(optarg);
    break;
  case 'd':
    DelayUs = (unsigned long)atoi(optarg);
    break;
  case 't':
    TableSize = (unsigned int)atoi(optarg);
    break;
  case 'k':
    HmacKey = optarg;
    break;
#endif
  default:
    return (usage());
    break;
  }

  argc -= optind;
#ifdef RSEC
  {
    unsigned long r;

    r = (unsigned long)time(0) * 2654435761UL ^ getpid();
    Sender[0] = r >> 24 & 0xff;
    Sender[1] = r >> 16 & 0xff;
    Sender[2] = r >> 8 & 0xff;
    Sender[3] = r & 0xff;
  }
#endif
  argv += optind;

  if (!lport || argc < 1)
    return (usage());

  /* parse peer addresses */
  Npeers = argc;
  if (!(Peers = malloc(Npeers * sizeof (*Peers)))) {
    perror("malloc");
    return (1);
  }
  memset(&hint, 0, sizeof (hint));
  hint.ai_family = AF_UNSPEC;
  hint.ai_socktype = SOCK_DGRAM;
  for (i = 0; i < argc; ++i) {
    char *host;
    char *port;
    char *sep;

    host = argv[i];
    if ((sep = strrchr(host, ':'))) {
      *sep = '\0';
      port = sep + 1;
    } else {
      fprintf(stderr, "invalid peer: %s (need host:port)\n", argv[i]);
      return (1);
    }
    if (getaddrinfo(host, port, &hint, &addr) || !addr) {
      fprintf(stderr, "getaddrinfo(%s:%s) failed\n", host, port);
      return (1);
    }
    memcpy(&Peers[i].addr, addr->ai_addr, addr->ai_addrlen);
    Peers[i].len = addr->ai_addrlen;
    freeaddrinfo(addr);
  }

  /* create and bind IPv4 socket */
  fd4 = -1;
  memset(&hint, 0, sizeof (hint));
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_DGRAM;
  hint.ai_flags = AI_PASSIVE;
  if (!getaddrinfo(0, lport, &hint, &addr) && addr) {
    if ((fd4 = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol)) >= 0) {
      i = 1;
      setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, &i, sizeof (i));
      if (bind(fd4, addr->ai_addr, addr->ai_addrlen)) {
        close(fd4);
        fd4 = -1;
      }
    }
    freeaddrinfo(addr);
  }

  /* create and bind IPv6 socket */
  fd6 = -1;
  memset(&hint, 0, sizeof (hint));
  hint.ai_family = AF_INET6;
  hint.ai_socktype = SOCK_DGRAM;
  hint.ai_flags = AI_PASSIVE;
  if (!getaddrinfo(0, lport, &hint, &addr) && addr) {
    if ((fd6 = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol)) >= 0) {
      i = 1;
      setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &i, sizeof (i));
      i = 1;
      setsockopt(fd6, IPPROTO_IPV6, IPV6_V6ONLY, &i, sizeof (i));
      if (bind(fd6, addr->ai_addr, addr->ai_addrlen)) {
        close(fd6);
        fd6 = -1;
      }
    }
    freeaddrinfo(addr);
  }

  if (fd4 < 0 && fd6 < 0) {
    fprintf(stderr, "%s: could not bind port %s\n", ProgName, lport);
    return (1);
  }

  chanInit(realloc, free);

  /* create datagram context */
  if (!(ctx = chanBlbTrnFdDatagramCtx(realloc, free))) {
    perror("chanBlbTrnFdDatagramCtx");
    return (1);
  }

  /* create channels */
  if (!(InChan = chanCreate(free, 0))) {
    perror("chanCreate");
    return (1);
  }
  if (!(OutChan = chanCreate(free, 0))) {
    perror("chanCreate");
    return (1);
  }

  /* the transport's direction contexts: a refusal is returned, and
   * chanBlb does not check for one */
  if (!(oc = chanBlbTrnFdDatagramOutputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0))
   || !(ic = chanBlbTrnFdDatagramInputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0))) {
    perror("chanBlbTrnFdDatagramOutputCtx/InputCtx");
    return (1);
  }

#ifdef RSEC
  {
    static struct chanBlbChnRsecEgrCtx egrCtx;
    static struct chanBlbChnRsecIgrCtx igrCtx;
    static struct hmacKeyCtx egrKeyCtx; /* one per direction: no callback */
    static struct hmacKeyCtx igrKeyCtx; /* context crosses threads */

    egrCtx.tagSize = igrCtx.tagSize = TagSize;
    egrCtx.dgramMax = igrCtx.dgramMax = DgramMax;
    egrCtx.tableSize = igrCtx.tableSize = TableSize;
    if (HmacKey) {
      egrKeyCtx.key = igrKeyCtx.key = (const unsigned char *)HmacKey;
      egrKeyCtx.keyLen = igrKeyCtx.keyLen = strlen(HmacKey);
      egrCtx.dtgHashSize = igrCtx.dtgHashSize = RMD128_SZ;
      egrCtx.dtgHashCtx = &egrKeyCtx;
      igrCtx.dtgHashCtx = &igrKeyCtx;
      egrCtx.dtgHashSign = gateSignCb;
      igrCtx.dtgHashVrfy = gateVrfyCb;
    }
    /* opaque is zero (file-scope statics) -- framer threads init/fini */
    /* start chanBlb with RSEC framing for both directions */
    if (!chanBlb(realloc, free
        ,OutChan, oc, chanBlbTrnFdDatagramOutput, outClose, &egrCtx, chanBlbChnRsecEgr
        ,InChan, ic, chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, &igrCtx, chanBlbChnRsecIgr, 0
        ,ctx, chanBlbTrnFdDatagramFinalClose
        ,0)) {
      perror("chanBlb");
      return (1);
    }
  }
#else
  /* start chanBlb for both directions */
  if (!chanBlb(realloc, free
      ,OutChan, oc, chanBlbTrnFdDatagramOutput, outClose, 0, 0
      ,InChan, ic, chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, 0, 0, 0
      ,ctx, chanBlbTrnFdDatagramFinalClose
      ,0)) {
    perror("chanBlb");
    return (1);
  }
#endif

  /* start display thread */
  if (pthread_create(&dt, 0, displayT, 0)) {
    perror("pthread_create");
    return (1);
  }

  /* main loop: read stdin, broadcast to all peers */
  {
    char line[1024];
    chanArr_t p[1];

    p[0].c = OutChan;
    p[0].o = chanOpPut;
    while (fgets(line, sizeof (line), stdin)) {
      unsigned int len;

      len = strlen(line);
#ifdef RSEC
      ++TagCounter;
#endif
      for (i = 0; i < Npeers; ++i) {
        chanBlb_t *m;
        unsigned int al;
#ifdef RSEC
        unsigned int bl;
        unsigned int off;
#endif

        al = Peers[i].len;
#ifdef RSEC
        /* egress blob: [addrlen(1)][addr(al)][tag(TagSize)][m(1)][delay_us(4)][payload] */
        bl = 1 + al + TagSize + 5 + len;
        if (!(m = malloc(chanBlb_tSize(bl)))) {
          perror("malloc");
          break;
        }
        m->l = bl;
        m->b[0] = (unsigned char)al;
        memcpy(m->b + 1, &Peers[i].addr, al);
        off = 1 + al;
        memcpy(m->b + off, Sender, sizeof (Sender));
        m->b[off + 4] = TagCounter & 0xff;
        m->b[off + 5] = TagCounter >> 8 & 0xff;
        off += TagSize;
        m->b[off] = (unsigned char)ParityShards;
        m->b[off + 1] = (unsigned char)((DelayUs >> 24) & 0xff);
        m->b[off + 2] = (unsigned char)((DelayUs >> 16) & 0xff);
        m->b[off + 3] = (unsigned char)((DelayUs >> 8) & 0xff);
        m->b[off + 4] = (unsigned char)(DelayUs & 0xff);
        memcpy(m->b + off + 5, line, len);
#else
        if (!(m = malloc(chanBlb_tSize(1 + al + len)))) {
          perror("malloc");
          break;
        }
        m->l = 1 + al + len;
        m->b[0] = (unsigned char)al;
        memcpy(m->b + 1, &Peers[i].addr, al);
        memcpy(m->b + 1 + al, line, len);
#endif
        p[0].v = (void **)&m;
        if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPut) {
          free(m);
          break;
        }
      }
    }

    /* send leave message (empty) to all peers */
#ifdef RSEC
    ++TagCounter;
#endif
    for (i = 0; i < Npeers; ++i) {
      chanBlb_t *m;
      unsigned int al;
#ifdef RSEC
      unsigned int bl;
      unsigned int off;
#endif

      al = Peers[i].len;
#ifdef RSEC
      /* egress blob: [addrlen(1)][addr(al)][tag(TagSize)][m(1)][delay_us(4)] */
      bl = 1 + al + TagSize + 5;
      if (!(m = malloc(chanBlb_tSize(bl)))) {
        perror("malloc");
        break;
      }
      m->l = bl;
      m->b[0] = (unsigned char)al;
      memcpy(m->b + 1, &Peers[i].addr, al);
      off = 1 + al;
      memcpy(m->b + off, Sender, sizeof (Sender));
      m->b[off + 4] = TagCounter & 0xff;
      m->b[off + 5] = TagCounter >> 8 & 0xff;
      off += TagSize;
      m->b[off] = (unsigned char)ParityShards;
      m->b[off + 1] = (unsigned char)((DelayUs >> 24) & 0xff);
      m->b[off + 2] = (unsigned char)((DelayUs >> 16) & 0xff);
      m->b[off + 3] = (unsigned char)((DelayUs >> 8) & 0xff);
      m->b[off + 4] = (unsigned char)(DelayUs & 0xff);
#else
      if (!(m = malloc(chanBlb_tSize(1 + al)))) {
        perror("malloc");
        break;
      }
      m->l = 1 + al;  /* no message content = leave */
      m->b[0] = (unsigned char)al;
      memcpy(m->b + 1, &Peers[i].addr, al);
#endif
      p[0].v = (void **)&m;
      if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPut)
        free(m);
    }
  }

  chanShut(OutChan);
  pthread_join(dt, 0);
  chanClose(OutChan);
  chanClose(InChan);
  free(Peers);
  return (0);
}
