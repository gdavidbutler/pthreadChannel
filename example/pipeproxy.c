/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2020 G. David Butler <gdb@dbSystems.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "chan.h"
#include "chanBlb.h"

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

int
main(
){
  chanBlb_t *m;
  chan_t *c[2];
  int p[2];
  pthread_t t;
  int i;

  chanInit(realloc, free);
  if (!(c[0] = chanCreate(0,0,0))
   || !(c[1] = chanCreate(0,0,0))) {
    perror("chanCreate");
    return (1);
  }
  if (pipe(p)) {
    perror("pipe");
    return (1);
  }
  if (!chanPipe(c[0], c[1], p[0], p[1], chanBlbFrmNs, 0, 0)) {
    perror("chanPipe");
    return (1);
  }
  chanOpen(c[0]);
  if (pthread_create(&t, 0, outT, c[0])) {
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
  chanShut(c[1]);
  pthread_join(t, 0);
  return (0);
}
