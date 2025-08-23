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

#ifndef __CHANSTRFIFO_H__
#define __CHANSTRFIFO_H__

chanSs_t
chanStrFIFOa(
  void *(*realloc)(void *, unsigned long)
 ,void (*free)(void *)
 ,void (*dequeue)(void *)
 ,int (*wake)(void *, chanSs_t)
 ,void *wakeClosure
 ,chanSd_t *deallocation
 ,chanSi_t *implementation
 ,void **storeClosure
 ,va_list list
/* unsigned int size */
);

#endif /* __CHANSTRFIFO_H__ */
