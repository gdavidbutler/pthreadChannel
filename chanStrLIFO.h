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

#ifndef __CHANSTRLIFO_H__
#define __CHANSTRLIFO_H__

/* chan store context */
typedef struct chanStrLIFOc chanStrLIFOc_t;

chanSs_t
chanStrLIFOa(
  chanStrLIFOc_t **context
 ,void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,void (*dequeue)(void *)
 ,unsigned int size
);

void
chanStrLIFOd(
  chanStrLIFOc_t *context
 ,chanSs_t state
);

chanSs_t
chanStrLIFOi(
  chanStrLIFOc_t *context
 ,chanSo_t operation
 ,chanSw_t waiting
 ,void **value
);

#endif /* __CHANSTRLIFO_H__ */
