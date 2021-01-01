/******************************************************************************
 *
 * APIs to decode parameters and bind them to SQLite statement, and to fetch
 * rows and encode them.
 *
 * The dqlite wire format for a list of parameters to be bound to a statement
 * is divided in header and body. The format of the header is:
 *
 *  8 bits: Number of parameters to bind (min is 1, max is 255).
 *  4 bits: Type code of the 1st parameter to bind.
 *  4 bits: Type code of the 2nd parameter to bind, or 0.
 *  4 bits: Type code of the 3rn parameter to bind, or 0.
 *  ...
 *
 * This repeats until reaching a full 64-bit word. If there are more than 14
 * parameters, the header will grow additional 64-bit words as needed, following
 * the same pattern: a sequence of 4-bit slots with type codes of the parameters
 * to bind, followed by a sequence of zero bits, until word boundary is reached.
 *
 * After the parameters header follows the parameters body, which contain one
 * value for each parameter to bind, following the normal encoding rules.
 *
 * The dqlite wire format for a set of query rows is divided in header and
 * body. The format of the header is:
 *
 *  64 bits: Number of columns in the result set (min is 1).
 *  64 bits: Name of the first column. If the name is longer, additional words
 *           of 64 bits can be used, like for normal string encoding.
 *  ...      If present, name of the 2nd, 3rd, ..., nth column.
 *
 * After the result set header follows the result set body, which is a sequence
 * of zero or more rows. Each row has the following format:
 *
 *  4 bits: Type code of the 1st column of the row.
 *  4 bits: Type code of the 2nd column of row, or 0.
 *  4 bits: Type code of the 2nd column of row, or 0.
 *
 * This repeats until reaching a full 64-bit word. If there are more than 16 row
 * columns, the header will grow additional 64-bit words as needed, following
 * the same pattern. After this row preamble, the values of all columns of the
 * row follow, using the normal dqlite enconding conventions.
 *
 *****************************************************************************/

#ifndef DQLITE_STMT_H
#define DQLITE_STMT_H

#include <sqlite3.h>

#include "lib/registry.h"

/* Hold state for a single open SQLite database */
struct stmt
{
	size_t id;	   /* Statement ID */
	sqlite3_stmt *stmt;  /* Underlying SQLite statement handle */
};

/* Initialize a statement state object */
void stmt_init(struct stmt *s);

/* Close a statement state object, releasing all associated resources. */
void stmt_close(struct stmt *s);

/* No-op hash function (hashing is not supported for stmt). This is
 * required by the registry interface. */
const char *stmtHash(struct stmt *stmt);

/* TODO: change registry naming pattern */
#define stmt_init stmt_init
#define stmt_close stmt_close
#define stmtHash stmtHash

REGISTRY(stmtRegistry, stmt);

#endif /* DQLITE_STMT_H */
