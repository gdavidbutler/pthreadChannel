/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2024 G. David Butler <gdb@dbSystems.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"
#include "chanStrFIFO.h"

static void *
outT(
 void *v
){
  chanBlb_t *m;

  pthread_cleanup_push((void(*)(void*))chanClose, v);
  while (chanOp(0, v, (void **)&m, chanOpGet) == chanOsGet) {
    unsigned int l;
    int i;

    for (l = 0; l < m->l && (i = write(1, m->b + l, m->l - l)) > 0; l += i);
    free(m);
  }
  pthread_cleanup_pop(1); /* chanClose(v) */
  return (0);
}

static unsigned int
input(
 void *v
,void *b
,unsigned int l
){
  int i;

  if ((i = read((int)(long)v, b, l)) < 0)
    i = 0;
  return (i);
}

static unsigned int
output(
 void *v
,const void *b
,unsigned int l
){
  int i;

  if ((i = write((int)(long)v, b, l)) < 0)
    i = 0;
  return (i);
}

static void
cls(
 void *v
){
  close((int)(long)v);
}

int
main(
  void
){
  chanBlb_t *m;
  chan_t *c[2];
  chanStrFIFOc_t *v;
  int p[2];
  pthread_t t;
  int i;

  chanInit(realloc, free);
  if (!(i = chanStrFIFOa(&v, realloc, free, free, 16))
   || !(c[0] = chanCreate(v, (chanSd_t)chanStrFIFOd, (chanSi_t)chanStrFIFOi, i))) {
    if (v)
      chanStrFIFOd(v, 0);
    perror("chanCreate");
    return (1);
  }
  if (!(i = chanStrFIFOa(&v, realloc, free, free, 16))
   || !(c[1] = chanCreate(v, (chanSd_t)chanStrFIFOd, (chanSi_t)chanStrFIFOi, i))) {
    if (v)
      chanStrFIFOd(v, 0);
    chanClose(c[0]);
    perror("chanCreate");
    return (1);
  }
  if (pipe(p)) {
    chanClose(c[1]);
    chanClose(c[0]);
    perror("pipe");
    return (1);
  }
  if (!chanBlb(realloc, free, c[0], (void *)(long)p[0], input, cls, c[1], (void *)(long)p[1], output, cls, 0, 0, 0, chanBlbFrmNs, 0, 0)) {
    close(p[1]);
    close(p[0]);
    chanClose(c[1]);
    chanClose(c[0]);
    perror("chanPipe");
    return (1);
  }
  if (pthread_create(&t, 0, outT, c[0])) {
    chanShut(c[1]);
    chanClose(c[1]);
    chanOp(0, c[0], 0, chanOpSht);
    chanClose(c[0]);
    perror("pthread_create");
    return (1);
  }
  while ((m = malloc(chanBlb_tSize(BUFSIZ)))
   && (i = read(0, m->b, BUFSIZ)) > 0) {
    void *t;

    m->l = i;
    if ((t = realloc(m, chanBlb_tSize(m->l))))
      m = t;
    if (chanOp(0, c[1], (void **)&m, chanOpPut) != chanOsPut) {
      perror("chanOpPut");
      return (1);
    }
  }
  free(m);
  chanShut(c[1]);
  chanClose(c[1]);
  pthread_join(t, 0);
  return (0);
}
