/*
 * pthreadChannel - an implementation of channels for pthreads
 * Copyright (C) 2016-2023 G. David Butler <gdb@dbSystems.com>
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

#ifndef __CHANSTR_H__
#define __CHANSTR_H__

/*
 * Channel FIFO Store
 */

/* allocate a context of items */
void *
chanFifoSa(
  void (*lastCloseItemCb)(void *)
 ,unsigned int size
); /* returns 0 on failure */

/* deallocate context */
void
chanFifoSd(
  void *context
 ,chanSs_t state
);

/* the chan implementation */
chanSs_t
chanFifoSi(
  void *context
 ,chanSo_t operation
 ,chanSw_t wait
 ,void **value
);

/*
 * Channel Latency Sensitive FIFO
 */

/* allocate a context of items */
void *
chanFlsoSa(
  void (*lastCloseItemCb)(void *)
 ,unsigned int max
 ,unsigned int initial
); /* returns 0 on failure */

/* deallocate context */
void
chanFlsoSd(
  void *context
 ,chanSs_t state
);

/* the chan implementation */
chanSs_t
chanFlsoSi(
  void *context
 ,chanSo_t operation
 ,chanSw_t wait
 ,void **value
);

/*
 * Channel LIFO Store
 */

/* allocate a context of items */
void *
chanLifoSa(
  void (*lastCloseItemCb)(void *)
 ,unsigned int size
); /* returns 0 on failure */

/* deallocate context */
void
chanLifoSd(
  void *context
 ,chanSs_t state
);

/* the chan implementation */
chanSs_t
chanLifoSi(
  void *context
 ,chanSo_t operation
 ,chanSw_t wait
 ,void **value
);

#endif /* __CHANSTR_H__ */
