/**
 * Step through a query progressively encoding a the row tuples.
 */

#ifndef QUERY_H_
#define QUERY_H_

#include <sqlite3.h>

#include "lib/serialize.h"
#include "lib/buffer.h"

/**
 * Step through the given query statement progressively encoding the yielded row
 * tuples, either until #SQLITE_DONE is returned or a full page of the given
 * buffer is filled.
 */
int queryBatch(sqlite3_stmt *stmt, struct buffer *buffer);

#endif /* QUERY_H_*/
