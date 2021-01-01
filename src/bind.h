/**
 * Bind statement parameters decoding them from a client request payload.
 */

#ifndef BIND_H_
#define BIND_H_

#include <sqlite3.h>

#include "lib/serialize.h"

/**
 * Bind the parameters of the given statement by decoding the given payload.
 */
int bindParams(sqlite3_stmt *stmt, struct cursor *cursor);

#endif /* BIND_H_*/
