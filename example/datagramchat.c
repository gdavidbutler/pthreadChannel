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
static unsigned int TagSize = 2;
static unsigned int DgramMax = 508;
static unsigned int TableSize = 64;
static unsigned int ParityShards = 1;
static unsigned int DelayMs = 1;
static unsigned short TagCounter;
static char *HmacKey;

/* rmd128hmac HMAC callbacks for chanBlbChnRsec */
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
#endif

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
    /* ingress blob: [addrlen(1)][addr(al)][tag(TagSize)][m(1)][payload] */
    skip = TagSize + 1;
#else
    skip = 0;
#endif
    ml = m->l - 1 - al - skip;
    if (1 + al + skip <= m->l
     && !getnameinfo((struct sockaddr *)(m->b + 1), al
         ,host, sizeof (host), serv, sizeof (serv)
         ,NI_NUMERICHOST | NI_NUMERICSERV)) {
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
   " [-m parity] [-f dgrammax] [-d delayms] [-t tablesize] [-k key]"
#endif
   " peer:port [peer:port ...]\n"
   "  Broadcast chat using unbound datagram sockets\n"
#ifdef RSEC
   "  -m  parity shards (default 1)\n"
   "  -f  max datagram payload size (default 548, IPv4/UDP fragment-free)\n"
   "  -d  inter-shard delay ms (default 1)\n"
   "  -t  ingress table size (default 64)\n"
   "  -k  HMAC shared key (enables rmd128 authentication)\n"
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
    ParityShards = (unsigned int)atoi(optarg);
    break;
  case 'f':
    DgramMax = (unsigned int)atoi(optarg);
    break;
  case 'd':
    DelayMs = (unsigned int)atoi(optarg);
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

#ifdef RSEC
  {
    static struct chanBlbChnRsecCtx rsecCtx;
    static struct hmacKeyCtx hmacKeyCtx;

    rsecCtx.tagSize = TagSize;
    rsecCtx.dgramMax = DgramMax;
    rsecCtx.tableSize = TableSize;
    if (HmacKey) {
      hmacKeyCtx.key = (const unsigned char *)HmacKey;
      hmacKeyCtx.keyLen = strlen(HmacKey);
      rsecCtx.hmacSize = RMD128_SZ;
      rsecCtx.hmacCtx = &hmacKeyCtx;
      rsecCtx.hmacSign = hmacSignCb;
      rsecCtx.hmacVrfy = hmacVrfyCb;
    }
    /* start chanBlb with RSEC framing for both directions */
    if (!chanBlb(realloc, free
        ,OutChan, chanBlbTrnFdDatagramOutputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0), chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, &rsecCtx, chanBlbChnRsecEgr
        ,InChan, chanBlbTrnFdDatagramInputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0), chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, &rsecCtx, chanBlbChnRsecIgr, 0
        ,ctx, chanBlbTrnFdDatagramFinalClose
        ,0)) {
      perror("chanBlb");
      return (1);
    }
  }
#else
  /* start chanBlb for both directions */
  if (!chanBlb(realloc, free
      ,OutChan, chanBlbTrnFdDatagramOutputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0), chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, 0, 0
      ,InChan, chanBlbTrnFdDatagramInputCtx(ctx, &fd4, fd6 >= 0 ? &fd6 : 0, fd4 >= 0 ? 1 : 0, fd6 >= 0 ? 1 : 0), chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, 0, 0, 0
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
        /* egress blob: [addrlen(1)][addr(al)][tag(TagSize)][m(1)][delay_ms(1)][payload] */
        bl = 1 + al + TagSize + 2 + len;
        if (!(m = malloc(chanBlb_tSize(bl)))) {
          perror("malloc");
          break;
        }
        m->l = bl;
        m->b[0] = (unsigned char)al;
        memcpy(m->b + 1, &Peers[i].addr, al);
        off = 1 + al;
        m->b[off] = (unsigned char)(TagCounter & 0xff);
        m->b[off + 1] = (unsigned char)((TagCounter >> 8) & 0xff);
        off += TagSize;
        m->b[off] = (unsigned char)ParityShards;
        m->b[off + 1] = (unsigned char)DelayMs;
        memcpy(m->b + off + 2, line, len);
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
      /* egress blob: [addrlen(1)][addr(al)][tag(TagSize)][m(1)][delay_ms(1)] */
      bl = 1 + al + TagSize + 2;
      if (!(m = malloc(chanBlb_tSize(bl)))) {
        perror("malloc");
        break;
      }
      m->l = bl;
      m->b[0] = (unsigned char)al;
      memcpy(m->b + 1, &Peers[i].addr, al);
      off = 1 + al;
      m->b[off] = (unsigned char)(TagCounter & 0xff);
      m->b[off + 1] = (unsigned char)((TagCounter >> 8) & 0xff);
      off += TagSize;
      m->b[off] = (unsigned char)ParityShards;
      m->b[off + 1] = (unsigned char)DelayMs;
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
  chanShut(InChan);
  pthread_join(dt, 0);
  chanClose(OutChan);
  chanClose(InChan);
  free(Peers);
  return (0);
}
