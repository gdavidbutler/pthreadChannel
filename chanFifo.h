/*
 * pthreadChannel - an implementation of CSP channels for pthreads
 * Copyright (C) 2019 G. David Butler <gdb@dbSystems.com>
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

#ifndef __CHANFIFO_H__
#define __CHANFIFO_H__

/*
 * Channel FIFO Store
 *
 * When threads talk more and work less,
 * a store of more than one message may be desired.
 */

/* opaque context */
typedef struct chanFifoSc
chanFifoSc_t;

/* allocate a context of size chan messages (void *) */
chanFifoSc_t *
chanFifoSa(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,unsigned int size
); /* returns 0 on failure */

/* deallocate context */
void
chanFifoSd(
  void *
);

/* the chan implementation */
chanSs_t
chanFifoSi(
  void *
 ,chanSo_t
 ,void **
);

#endif /* __CHANFIFO_H__ */
