#include <sqlite3.h>

#include "./lib/assert.h"
#include "./tuple.h"

#include "stmt.h"

/* The maximum number of columns we expect (for bindings or rows) is 255, which
 * can fit in one byte. */
#define STMT_MAX_COLUMNS (1 << 8) - 1

void stmt_init(struct stmt *s)
{
	s->stmt = NULL;
}

void stmt_close(struct stmt *s)
{
	if (s->stmt != NULL) {
		/* Ignore the return code, since it will be non-zero in case the
		 * most rececent evaluation of the statement failed. */
		sqlite3_finalize(s->stmt);
	}
}

const char *stmtHash(struct stmt *stmt)
{
	(void)stmt;
	return NULL;
}

REGISTRY_METHODS(stmtRegistry, stmt);
