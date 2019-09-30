/*
 * pthreadChannel - an implementation of CSP/agent channels for pthreads
 * Copyright (C) 2018 G. David Butler <gdb@dbSystems.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Based on https://swtch.com/libtask/tcpproxy.c */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netdb.h>
#include "chan.h"
#include "chanSock.h"

struct addrinfo *Caddr;

/* connect two chanSocks back to back, with read and write channels reversed */
/* I know, it's much more efficient to create two threads with read() / write() loops... */
static void *
servT(
  void *v
){
  void *t;
  int f[2];
  chan_t *c[2];
  chanPoll_t p[2];

  f[0] = (int)(long)v;
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)f[0]);
  if ((f[1] = socket(Caddr->ai_family, Caddr->ai_socktype, Caddr->ai_protocol)) < 0) {
    perror("socket");
    goto exit0;
  }
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)f[1]);
  if (connect(f[1], Caddr->ai_addr, Caddr->ai_addrlen)) {
    perror("connect");
    goto exit1;
  }
  if (!(c[0] = chanCreate(realloc, free, 0, 0, 0))) {
    perror("chanCreate");
    goto exit1;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, c[0]);
  if (!(c[1] = chanCreate(realloc, free, 0, 0, 0))) {
    perror("chanCreate");
    goto exit2;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, c[1]);
  if (!(p[0].c = chanSock(realloc, free, f[0], c[0], c[1], 65535))) {
    perror("chanSock");
    goto exit3;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, p[0].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[0].c);
  if (!(p[1].c = chanSock(realloc, free, f[1], c[1], c[0], 65535))) {
    perror("chanSock");
    goto exit4;
  }
  pthread_cleanup_push((void(*)(void*))chanClose, p[1].c);
  pthread_cleanup_push((void(*)(void*))chanShut, p[1].c);
  /* wait for either chanSock to chanShut */
  p[0].v = p[1].v = &t;
  p[0].o = p[1].o = chanPoGet;
  chanPoll(-1, sizeof (p) / sizeof (p[0]), p);
  pthread_cleanup_pop(1); /* chanShut(p[1].c) */
  pthread_cleanup_pop(1); /* chanClose(p[1].c) */
exit4:
  pthread_cleanup_pop(1); /* chanShut(p[0].c) */
  pthread_cleanup_pop(1); /* chanClose(p[0].c) */
exit3:
  pthread_cleanup_pop(1); /* chanClose(c[1]) */
exit2:
  pthread_cleanup_pop(1); /* chanClose(c[0]) */
exit1:
  pthread_cleanup_pop(1); /* close(f[1]) */
exit0:
  pthread_cleanup_pop(1); /* close(f[0]) */
  return (0);
}

static void *
listenT(
  void *v
){
  struct sockaddr a;
  socklen_t s;
  int l;
  int c;

  l = (int)(long)v;
  pthread_cleanup_push((void(*)(void*))close, (void *)(long)l);
  if (listen(l, 1)) {
    perror("listen");
    goto exit0;
  }
  while ((c = accept(l, &a, &s)) >= 0) {
    pthread_t t;

    if (pthread_create(&t, 0, servT, (void *)(long)c)) {
      close(c);
      perror("chanIt");
    }
    pthread_detach(t);
  }
  perror("accept");
exit0:
  pthread_cleanup_pop(1); /* close(l) */
  return (0);
}

static char const *ProgName;

static int
usage(
  void
){
  fprintf(stderr
  ,"Usage: %s"
   " [-T socktype] [-F family] [-P protocol] [-H host] -S service"
   " [-t socktype] [-f family] [-p protocol] -h host -s service"
   "\n"
  ,ProgName
  );
  return (1);
}

int
main(
  int argc
 ,char *argv[]
){
  struct addrinfo shint;
  struct addrinfo chint;
  char *shost;
  char *sserv;
  char *chost;
  char *cserv;
  struct addrinfo *saddr;
  int i;
  int fd;
  pthread_t p;

  ProgName = argv[0];

  memset(&shint, 0, sizeof (shint));
  shint.ai_flags |= AI_PASSIVE;
  memset(&chint, 0, sizeof (chint));
  shost = sserv = 0;
  chost = cserv = 0;

  while ((i = getopt(argc, argv, "T:F:P:H:S:t:f:p:h:s:")) != -1) switch (i) {
  case 'T':
    if (!(shint.ai_socktype = atoi(optarg)))
      return (usage());
    break;
  case 'F':
    if (!(shint.ai_family = atoi(optarg)))
      return (usage());
    break;
  case 'P': {
    struct protoent *p;

    p = getprotobyname(optarg);
    if (!(shint.ai_protocol = p->p_proto))
      return (usage());
    } break;
  case 'H':
    shost = optarg;
    break;
  case 'S':
    sserv = optarg;
    break;
  case 't':
    if (!(chint.ai_socktype = atoi(optarg)))
      return (usage());
    break;
  case 'f':
    if (!(chint.ai_family = atoi(optarg)))
      return (usage());
    break;
  case 'p': {
    struct protoent *p;

    p = getprotobyname(optarg);
    if (!(chint.ai_protocol = p->p_proto))
      return (usage());
    } break;
  case 'h':
    chost = optarg;
    break;
  case 's':
    cserv = optarg;
    break;
  default:
    return (usage());
    break;
  }

  argc -= optind;
  argv += optind;

  if (argc || !sserv || !chost || !cserv)
    return (usage());

  if (getaddrinfo(shost, sserv, &shint, &saddr) || !saddr) {
    perror("getaddrinfo(server)");
    return (1);
  }
  if (saddr->ai_next) {
    fprintf(stderr, "too many server addrinfo found, be more specific\n");
    return (1);
  }
  if (getaddrinfo(chost, cserv, &chint, &Caddr) || !Caddr) {
    perror("getaddrinfo(client)");
    return (1);
  }
  if (Caddr->ai_next) {
    fprintf(stderr, "too many client addrinfo found, be more specific\n");
    return (1);
  }
  i = 1;
  if ((fd = socket(saddr->ai_family, saddr->ai_socktype, saddr->ai_protocol)) < 0
   || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof (i))
   || bind(fd, saddr->ai_addr, saddr->ai_addrlen)) {
    perror("socket/setsockopt/bind");
    return (1);
  }
  if ((saddr->ai_socktype == SOCK_STREAM || saddr->ai_socktype == SOCK_SEQPACKET)) {
    if (pthread_create(&p, 0, listenT, (void *)(long)fd)) {
      perror("pthread_create");
      return (1);
    }
  } else {
    if (pthread_create(&p, 0, servT, (void *)(long)fd)) {
      perror("pthread_create");
      return (1);
    }
  }
  pthread_join(p, 0);
  freeaddrinfo(saddr);
  freeaddrinfo(Caddr);
  return (0);
}
