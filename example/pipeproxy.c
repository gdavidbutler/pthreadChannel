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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chan.h"
#include "chanBlb.h"

int
main(
){
  chanBlb_t *m;
  chan_t *c[2];
  int p[2];

  if (!(c[0] = chanCreate(0,0, 0,0,0))
   || !(c[1] = chanCreate(0,0, 0,0,0))) {
    perror("chanCreate");
    return (1);
  }
  if (pipe(p)) {
    perror("pipe");
    return (1);
  }
  if (!chanPipe(0,0, c[0], c[1], p[0], p[1], 0)) {
    perror("chanPipe");
    return (1);
  }
  while ((m = malloc(sizeof (*m) - sizeof (m->l) + BUFSIZ))
   && fgets((char *)m->b, BUFSIZ, stdin)) {
    void *t;

    m->l = strlen((char *)m->b);
    if ((t = realloc(m, sizeof (*m) - sizeof (m->l) + m->l)))
      m = t;
    if (chanPut(-1, c[1], m) != chanOsPut) {
      perror("chanPut");
      return (1);
    }
    if (chanGet(-1, c[0], (void **)&m) != chanOsGet) {
      perror("chanGet");
      return (1);
    }
    printf("%.*s", m->l, m->b);
    free(m);
  }
  return (0);
}
