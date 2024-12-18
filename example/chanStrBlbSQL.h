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

#ifndef __CHANSTRBLBSQL_H__
#define __CHANSTRBLBSQL_H__

/* chan store context */
typedef struct chanStrBlbSQLc chanStrBlbSQLc_t;

chanSs_t
chanStrBlbSQLa(
  chanStrBlbSQLc_t **context
 ,const char *path
 ,unsigned int journal
 ,unsigned int synchronous
 ,sqlite3_int64 limit
);

void
chanStrBlbSQLd(
  chanStrBlbSQLc_t *context
 ,chanSs_t state
);

chanSs_t
chanStrBlbSQLi(
  chanStrBlbSQLc_t *context
 ,chanSo_t operation
 ,chanSw_t wait
 ,chanBlb_t **value
);

#endif /* __CHANSTRBLBSQL_H__ */
