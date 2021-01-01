#include "query.h"
#include "tuple.h"


/* Return the type code of the i'th column value.
 *
 * TODO: find a better way to handle time types. */
static int valueType(sqlite3_stmt *stmt, int i)
{
	int type = sqlite3_column_type(stmt, i);
	const char *columnTypeName = sqlite3_column_decltype(stmt, i);
	if (columnTypeName != NULL) {
		if ((strcasecmp(columnTypeName, "DATETIME") == 0) ||
		    (strcasecmp(columnTypeName, "DATE") == 0) ||
		    (strcasecmp(columnTypeName, "TIMESTAMP") == 0)) {
			if (type == SQLITE_INTEGER) {
				type = DQLITE_UNIXTIME;
			} else {
				assert(type == SQLITE_TEXT ||
				       type == SQLITE_NULL);
				type = DQLITE_ISO8601;
			}
		} else if (strcasecmp(columnTypeName, "BOOLEAN") == 0) {
			assert(type == SQLITE_INTEGER || type == SQLITE_NULL);
			type = DQLITE_BOOLEAN;
		}
	}

	assert(type < 16);
	return type;
}

/* Append a single row to the message. */
static int encodeRow(sqlite3_stmt *stmt, struct buffer *buffer, int n)
{
	struct tupleEncoder encoder;
	int rc;
	int i;

	rc = tupleEncoderInit(&encoder, (unsigned)n, TUPLE_ROW, buffer);
	if (rc != 0) {
		return SQLITE_ERROR;
	}

	/* Encode the row values */
	for (i = 0; i < n; i++) {
		/* Figure the type */
		struct value value;
		value.type = valueType(stmt, i);
		switch (value.type) {
			case SQLITE_INTEGER:
				value.integer =
				    sqlite3_column_int64(stmt, i);
				break;
			case SQLITE_FLOAT:
				value.float_ =
				    sqlite3_column_double(stmt, i);
				break;
			case SQLITE_BLOB:
				value.blob.base = (char*)sqlite3_column_blob(stmt, i);
				value.blob.len = (size_t)sqlite3_column_bytes(stmt, i);
				break;
			case SQLITE_NULL:
				/* TODO: allow null to be encoded with 0 bytes
				 */
				value.null = 0;
				break;
			case SQLITE_TEXT:
				value.text =
				    (text_t)sqlite3_column_text(stmt, i);
				break;
			case DQLITE_UNIXTIME:
				value.integer =
				    sqlite3_column_int64(stmt, i);
				break;
			case DQLITE_ISO8601:
				value.text =
				    (text_t)sqlite3_column_text(stmt, i);
				if (value.text == NULL) {
					value.text = "";
				}
				break;
			case DQLITE_BOOLEAN:
				value.integer =
				    sqlite3_column_int64(stmt, i);
				break;
			default:
				return SQLITE_ERROR;
		}

		rc = tupleEncoderNext(&encoder, &value);
		if (rc != 0) {
			return rc;
		}
	}

	return SQLITE_OK;
}

int queryBatch(sqlite3_stmt *stmt, struct buffer *buffer)
{
	int n; /* Column count */
	int i;
	uint64_t n64;
	void *cursor;
	int rc;

	n = sqlite3_column_count(stmt);
	if (n <= 0) {
		return SQLITE_ERROR;
	}
	n64 = (uint64_t)n;

	/* Insert the column count */
	cursor = bufferAdvance(buffer, sizeof(uint64_t));
	assert(cursor != NULL);
	uint64Encode(&n64, &cursor);

	/* Insert the column names */
	for (i = 0; i < n; i++) {
		const char *name = sqlite3_column_name(stmt, i);
		cursor = bufferAdvance(buffer, textSizeof(&name));
		if (cursor == NULL) {
			return SQLITE_NOMEM;
		}
		textEncode(&name, &cursor);
	}

	/* Insert the rows. */
	do {
		if (bufferOffset(buffer) >= buffer->pageSize) {
			/* If we are already filled a memory page, let's break
			 * for now, we'll send more rows in a separate
			 * response. */
			rc = SQLITE_ROW;
			break;
		}
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_ROW) {
			break;
		}
		rc = encodeRow(stmt, buffer, n);
		if (rc != SQLITE_OK) {
			break;
		}

	} while (1);

	return rc;
}
