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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "chanBlbTrnFdDatagram.h"

struct peer {
  struct sockaddr_storage addr;
  socklen_t len;
};

static struct peer *Peers;
static int Npeers;
static chan_t *OutChan;
static chan_t *InChan;

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
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];

    al = m->b[0];
    ml = m->l - 1 - al;
    if (al <= m->l - 1
     && !getnameinfo((struct sockaddr *)(m->b + 1), al
         ,host, sizeof (host), serv, sizeof (serv)
         ,NI_NUMERICHOST | NI_NUMERICSERV)) {
      if (ml == 0)
        printf("[%s:%s]: left\n", host, serv);
      else
        printf("[%s:%s]: %.*s", host, serv, ml, m->b + 1 + al);
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
  ,"Usage: %s -l port peer:port [peer:port ...]\n"
   "  Broadcast chat using unbound datagram sockets\n"
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
  int fd;
  void *ctx;
  pthread_t dt;

  ProgName = argv[0];
  lport = 0;

  while ((i = getopt(argc, argv, "l:")) != -1) switch (i) {
  case 'l':
    lport = optarg;
    break;
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

  /* create and bind socket */
  memset(&hint, 0, sizeof (hint));
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_DGRAM;
  hint.ai_flags = AI_PASSIVE;
  if (getaddrinfo(0, lport, &hint, &addr) || !addr) {
    perror("getaddrinfo(listen)");
    return (1);
  }
  if ((fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol)) < 0) {
    perror("socket");
    return (1);
  }
  i = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof (i))) {
    perror("setsockopt");
    return (1);
  }
  if (bind(fd, addr->ai_addr, addr->ai_addrlen)) {
    perror("bind");
    return (1);
  }
  freeaddrinfo(addr);

  chanInit(realloc, free);

  /* create datagram context */
  if (!(ctx = chanBlbTrnFdDatagramCtx())) {
    perror("chanBlbTrnFdDatagramCtx");
    return (1);
  }
  chanBlbTrnFdDatagramInputCtx(ctx, fd);
  chanBlbTrnFdDatagramOutputCtx(ctx, fd);

  /* create channels */
  if (!(InChan = chanCreate(free, 0))) {
    perror("chanCreate");
    return (1);
  }
  if (!(OutChan = chanCreate(free, 0))) {
    perror("chanCreate");
    return (1);
  }

  /* start chanBlb for both directions */
  if (!chanBlb(realloc, free
      ,OutChan, ctx, chanBlbTrnFdDatagramOutput, chanBlbTrnFdDatagramOutputClose, 0, 0
      ,InChan, ctx, chanBlbTrnFdDatagramInput, chanBlbTrnFdDatagramInputClose, 0, 0, 0
      ,ctx, chanBlbTrnFdDatagramFinalClose
      ,0)) {
    perror("chanBlb");
    return (1);
  }

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
      for (i = 0; i < Npeers; ++i) {
        chanBlb_t *m;
        unsigned int al;

        al = Peers[i].len;
        if (!(m = malloc(chanBlb_tSize(1 + al + len)))) {
          perror("malloc");
          break;
        }
        m->l = 1 + al + len;
        m->b[0] = (unsigned char)al;
        memcpy(m->b + 1, &Peers[i].addr, al);
        memcpy(m->b + 1 + al, line, len);
        p[0].v = (void **)&m;
        if (chanOne(0, sizeof (p) / sizeof (p[0]), p) != 1 || p[0].s != chanOsPut) {
          free(m);
          break;
        }
      }
    }

    /* send leave message (empty) to all peers */
    for (i = 0; i < Npeers; ++i) {
      chanBlb_t *m;
      unsigned int al;

      al = Peers[i].len;
      if (!(m = malloc(chanBlb_tSize(1 + al)))) {
        perror("malloc");
        break;
      }
      m->l = 1 + al;  /* no message content = leave */
      m->b[0] = (unsigned char)al;
      memcpy(m->b + 1, &Peers[i].addr, al);
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
