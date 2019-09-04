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
 * Channel FIFO Queue
 *
 * When threads talk more and work less,
 * a queue of more than one message may be desired.
 */
typedef struct chanFifoQc chanFifoQc_t;
chanFifoQc_t *chanFifoQa(void *(*realloc)(void *, unsigned long), void (*free)(void *), unsigned int); /* returns 0 on failure */
void chanFifoQd(void *);
chanQs_t chanFifoQi(void *, chanQo_t, void **);

#endif /* __CHANFIFO_H__ */
