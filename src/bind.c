#include "bind.h"
#include "tuple.h"

/* Bind a single parameter. */
static int bindOne(sqlite3_stmt *stmt, int n, struct value *value)
{
	int rc;

	/* TODO: the binding calls below currently use SQLITE_TRANSIENT when
	 * passing pointers to data (for TEXT or BLOB datatypes). This way
	 * SQLite makes its private copy of the data before the bind call
	 * returns, and we can reuse the message body buffer. The overhead of
	 * the copy is typically low, but if it becomes a concern, this could be
	 * optimized to make no copy and instead prevent the message body from
	 * being reused. */
	switch (value->type) {
		case SQLITE_INTEGER:
			rc = sqlite3_bind_int64(stmt, n, value->integer);
			break;
		case SQLITE_FLOAT:
			rc = sqlite3_bind_double(stmt, n, value->float_);
			break;
		case SQLITE_BLOB:
			rc = sqlite3_bind_blob(stmt, n, value->blob.base,
					       (int)value->blob.len,
					       SQLITE_TRANSIENT);
			break;
		case SQLITE_NULL:
			rc = sqlite3_bind_null(stmt, n);
			break;
		case SQLITE_TEXT:
			rc = sqlite3_bind_text(stmt, n, value->text, -1,
					       SQLITE_TRANSIENT);
			break;
		case DQLITE_ISO8601:
			rc = sqlite3_bind_text(stmt, n, value->text, -1,
					       SQLITE_TRANSIENT);
			break;
		case DQLITE_BOOLEAN:
			rc = sqlite3_bind_int64(stmt, n,
						value->boolean == 0 ? 0 : 1);
			break;
		default:
			rc = DQLITE_PROTO;
			break;
	}

	return rc;
}

int bindParams(sqlite3_stmt *stmt, struct cursor *cursor)
{
	struct tupleDecoder decoder;
	unsigned i;
	int rc;

	sqlite3_reset(stmt);

	/* If the payload has been fully consumed, it means there are no
	 * parameters to bind. */
	if (cursor->cap == 0) {
		return 0;
	}

	rc = tupleDecoderInit(&decoder, 0, cursor);
	if (rc != 0) {
		return rc;
	}
	for (i = 0; i < tupleDecoderN(&decoder); i++) {
		struct value value;
		rc = tupleDecoderNext(&decoder, &value);
		if (rc != 0) {
			return rc;
		}
		rc = bindOne(stmt, (int)(i + 1), &value);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}
