#include <sqlite3.h>
#include <unistd.h>
#include <stdint.h>

#include "lib/assert.h"

#include "client.h"
#include "message.h"
#include "protocol.h"
#include "request.h"
#include "response.h"
#include "tuple.h"

int clientInit(struct client *c, int fd)
{
	int rv;
	c->fd = fd;

	rv = bufferInit(&c->read);
	if (rv != 0) {
		goto err;
	}
	rv = bufferInit(&c->write);
	if (rv != 0) {
		goto errAfterReadBufferInit;
	}

	return 0;

errAfterReadBufferInit:
	bufferClose(&c->read);
err:
	return rv;
}

void clientClose(struct client *c)
{
	bufferClose(&c->write);
	bufferClose(&c->read);
}

int clientSendHandshake(struct client *c)
{
	uint64_t protocol;
	ssize_t rv;

	/* TODO: update to version 1 */
	protocol = byteFlip64(DQLITE_PROTOCOL_VERSION_LEGACY);

	rv = write(c->fd, &protocol, sizeof protocol);
	if (rv < 0) {
		return DQLITE_ERROR;
	}

	return 0;
}

/* Write out a request. */
#define REQUEST(LOWER, UPPER)                                     \
	{                                                         \
		struct message message;                           \
		size_t n;                                         \
		size_t n1;                                        \
		size_t n2;                                        \
		void *cursor;                                     \
		ssize_t rv;                                       \
		n1 = messageSizeof(&message);                     \
		n2 = request##LOWER##Sizeof(&request);            \
		n = n1 + n2;                                      \
		bufferReset(&c->write);                           \
		cursor = bufferAdvance(&c->write, n);             \
		if (cursor == NULL) {                             \
			return DQLITE_NOMEM;                      \
		}                                                 \
		assert(n2 % 8 == 0);                              \
		message.type = DQLITE_REQUEST_##UPPER;            \
		message.words = (uint32_t)(n2 / 8);               \
		messageEncode(&message, &cursor);                 \
		request##LOWER##Encode(&request, &cursor);        \
		rv = write(c->fd, bufferCursor(&c->write, 0), n); \
		if (rv != (int)n) {                               \
			return DQLITE_ERROR;                      \
		}                                                 \
	}

/* Read a response without decoding it. */
#define READ(LOWER, UPPER)                                      \
	{                                                       \
		struct message _message;                        \
		struct cursor _cursor;                          \
		size_t _n;                                      \
		void *_p;                                       \
		ssize_t _rv;                                    \
		_n = messageSizeof(&_message);                  \
		bufferReset(&c->read);                          \
		_p = bufferAdvance(&c->read, _n);               \
		assert(_p != NULL);                             \
		_rv = read(c->fd, _p, _n);                      \
		if (_rv != (int)_n) {                           \
			return DQLITE_ERROR;                    \
		}                                               \
		_cursor.p = _p;                                 \
		_cursor.cap = _n;                               \
		_rv = messageDecode(&_cursor, &_message);       \
		assert(_rv == 0);                               \
		if (_message.type != DQLITE_RESPONSE_##UPPER) { \
			return DQLITE_ERROR;                    \
		}                                               \
		bufferReset(&c->read);                          \
		_n = _message.words * 8;                        \
		_p = bufferAdvance(&c->read, _n);               \
		if (_p == NULL) {                               \
			return DQLITE_ERROR;                    \
		}                                               \
		_rv = read(c->fd, _p, _n);                      \
		if (_rv != (int)_n) {                           \
			return DQLITE_ERROR;                    \
		}                                               \
	}

/* Decode a response. */
#define DECODE(LOWER)                                             \
	{                                                         \
		int rv;                                           \
		struct cursor cursor;                             \
		cursor.p = bufferCursor(&c->read, 0);             \
		cursor.cap = bufferOffset(&c->read);              \
		rv = response##LOWER##Decode(&cursor, &response); \
		if (rv != 0) {                                    \
			return DQLITE_ERROR;                      \
		}                                                 \
	}

/* Read and decode a response. */
#define RESPONSE(LOWER, UPPER) \
	READ(LOWER, UPPER);    \
	DECODE(LOWER)

int clientSendOpen(struct client *c, const char *name)
{
	struct requestopen request;
	request.filename = name;
	request.flags = 0;    /* TODO: this is unused, should we drop it? */
	request.vfs = "test"; /* TODO: this is unused, should we drop it? */
	REQUEST(open, OPEN);
	return 0;
}

int clientRecvDb(struct client *c)
{
	struct responsedb response;
	RESPONSE(db, DB);
	c->dbId = response.id;
	return 0;
}

int clientSendPrepare(struct client *c, const char *sql)
{
	struct requestprepare request;
	request.dbId = c->dbId;
	request.sql = sql;
	REQUEST(prepare, PREPARE);
	return 0;
}

int clientRecvStmt(struct client *c, unsigned *stmtId)
{
	struct responsestmt response;
	RESPONSE(stmt, STMT);
	*stmtId = response.id;
	return 0;
}

int clientSendExec(struct client *c, unsigned stmtId)
{
	struct requestexec request;
	request.dbId = c->dbId;
	request.stmtId = stmtId;
	REQUEST(exec, EXEC);
	return 0;
}

int clientSendExecSQL(struct client *c, const char *sql)
{
	struct requestexecSql request;
	request.dbId = c->dbId;
	request.sql = sql;
	REQUEST(execSql, EXEC_SQL);
	return 0;
}

int clientRecvResult(struct client *c,
		     unsigned *lastInsertId,
		     unsigned *rowsAffected)
{
	struct responseresult response;
	RESPONSE(result, RESULT);
	*lastInsertId = (unsigned)response.lastInsertId;
	*rowsAffected = (unsigned)response.rowsAffected;
	return 0;
}

int clientSendQuery(struct client *c, unsigned stmtId)
{
	struct requestquery request;
	request.dbId = c->dbId;
	request.stmtId = stmtId;
	REQUEST(query, QUERY);
	return 0;
}

int clientRecvRows(struct client *c, struct rows *rows)
{
	struct cursor cursor;
	struct tupleDecoder decoder;
	uint64_t columnCount;
	struct row *last;
	unsigned i;
	int rv;
	READ(rows, ROWS);
	cursor.p = bufferCursor(&c->read, 0);
	cursor.cap = bufferOffset(&c->read);
	rv = uint64Decode(&cursor, &columnCount);
	if (rv != 0) {
		return DQLITE_ERROR;
	}
	rows->columnCount = (unsigned)columnCount;
	for (i = 0; i < rows->columnCount; i++) {
		rows->columnNames = sqlite3_malloc(
		    (int)(columnCount * sizeof *rows->columnNames));
		if (rows->columnNames == NULL) {
			return DQLITE_ERROR;
		}
		rv = textDecode(&cursor, &rows->columnNames[i]);
		if (rv != 0) {
			return DQLITE_ERROR;
		}
	}
	last = NULL;
	while (1) {
		uint64_t eof;
		struct row *row;
		if (cursor.cap < 8) {
			/* No EOF marker fond */
			return DQLITE_ERROR;
		}
		eof = byteFlip64(*(uint64_t *)cursor.p);
		if (eof == DQLITE_RESPONSE_ROWS_DONE ||
		    eof == DQLITE_RESPONSE_ROWS_PART) {
			break;
		}
		row = sqlite3_malloc(sizeof *row);
		if (row == NULL) {
			return DQLITE_NOMEM;
		}
		row->values =
		    sqlite3_malloc((int)(columnCount * sizeof *row->values));
		if (row->values == NULL) {
			return DQLITE_NOMEM;
		}
		row->next = NULL;
		rv = tupleDecoderInit(&decoder, (unsigned)columnCount, &cursor);
		if (rv != 0) {
			return DQLITE_ERROR;
		}
		for (i = 0; i < rows->columnCount; i++) {
			rv = tupleDecoderNext(&decoder, &row->values[i]);
			if (rv != 0) {
				return DQLITE_ERROR;
			}
		}
		if (last == NULL) {
			rows->next = row;
		} else {
			last->next = row;
		}
		last = row;
	}
	return 0;
}

void clientCloseRows(struct rows *rows)
{
	struct row *row = rows->next;
	while (row != NULL) {
		struct row *next;
		next = row->next;
		sqlite3_free(row->values);
		sqlite3_free(row);
		row = next;
	}
	sqlite3_free(rows->columnNames);
}

int clientSendAdd(struct client *c, unsigned id, const char *address)
{
	struct requestadd request;
	request.id = id;
	request.address = address;
	REQUEST(add, ADD);
	return 0;
}

int clientSendAssign(struct client *c, unsigned id, int role)
{
	struct requestassign request;
	(void)role;
	/* TODO: actually send an assign request, not a legacy promote one. */
	request.id = id;
	REQUEST(assign, ASSIGN);
	return 0;
}

int clientSendRemove(struct client *c, unsigned id)
{
	struct requestremove request;
	request.id = id;
	REQUEST(remove, REMOVE);
	return 0;
}

int clientRecvEmpty(struct client *c)
{
	struct responseempty response;
	RESPONSE(empty, EMPTY);
	return 0;
}
