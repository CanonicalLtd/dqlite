#include "../../include/dqlite.h"
#include "../../src/gateway.h"
#include "../../src/request.h"
#include "../../src/response.h"
#include "../../src/tuple.h"
#include "../lib/cluster.h"
#include "../lib/runner.h"

TEST_MODULE(gateway);

/******************************************************************************
 *
 * Fixture.
 *
 ******************************************************************************/

/* Context for a gateway handle request. */
struct context
{
	bool invoked;
	int status;
	int type;
};

/* Drive a single gateway. Each gateway is associated with a different raft
 * node. */
struct connection
{
	struct gateway gateway;
	struct buffer buf1;   /* Request payload */
	struct buffer buf2;   /* Response payload */
	struct cursor cursor; /* Response read cursor */
	struct handle handle; /* Async handle request */
	struct context context;
};

#define FIXTURE                                   \
	FIXTURE_CLUSTER;                          \
	struct connection connections[N_SERVERS]; \
	struct gateway *gateway;                  \
	struct buffer *buf1;                      \
	struct cursor *cursor;                    \
	struct buffer *buf2;                      \
	struct handle *handle;                    \
	struct context *context;

#define SETUP                                                         \
	unsigned i;                                                   \
	int rc;                                                       \
	SETUP_CLUSTER(V2);                                            \
	for (i = 0; i < N_SERVERS; i++) {                             \
		struct connection *c = &f->connections[i];            \
		struct config *config;                                \
		config = CLUSTER_CONFIG(i);                           \
		config->pageSize = 512;                               \
		gatewayInit(&c->gateway, config, CLUSTER_REGISTRY(i), \
			    CLUSTER_RAFT(i));                         \
		c->handle.data = &c->context;                         \
		rc = bufferInit(&c->buf1);                            \
		munit_assert_int(rc, ==, 0);                          \
		rc = bufferInit(&c->buf2);                            \
		munit_assert_int(rc, ==, 0);                          \
	}                                                             \
	SELECT(0)

#define TEAR_DOWN                                          \
	unsigned i;                                        \
	for (i = 0; i < N_SERVERS; i++) {                  \
		struct connection *c = &f->connections[i]; \
		gatewayClose(&c->gateway);                 \
		bufferClose(&c->buf1);                     \
		bufferClose(&c->buf2);                     \
	}                                                  \
	TEAR_DOWN_CLUSTER;

static void handleCb(struct handle *req, int status, int type)
{
	struct context *c = req->data;
	c->invoked = true;
	c->status = status;
	c->type = type;
}

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Select which gateway to use for performing requests. */
#define SELECT(I)                                \
	f->gateway = &f->connections[I].gateway; \
	f->buf1 = &f->connections[I].buf1;       \
	f->buf2 = &f->connections[I].buf2;       \
	f->cursor = &f->connections[I].cursor;   \
	f->context = &f->connections[I].context; \
	f->handle = &f->connections[I].handle

/* Allocate the payload buffer, encode a request of the given lower case name
 * and initialize the fixture cursor. */
#define ENCODE(REQUEST, LOWER)                               \
	{                                                    \
		size_t n2 = request##LOWER##Sizeof(REQUEST); \
		void *cursor;                                \
		bufferReset(f->buf1);                        \
		cursor = bufferAdvance(f->buf1, n2);         \
		munit_assert_ptr_not_null(cursor);           \
		request##LOWER##Encode(REQUEST, &cursor);    \
	}

/* Encode N parameters with the given values */
#define ENCODE_PARAMS(N, VALUES)                                            \
	{                                                                   \
		struct tupleEncoder encoder;                                \
		int i2;                                                     \
		int rc2;                                                    \
		rc2 = tupleEncoderInit(&encoder, N, TUPLE_PARAMS, f->buf1); \
		munit_assert_int(rc2, ==, 0);                               \
		for (i2 = 0; i2 < N; i2++) {                                \
			rc2 = tupleEncoderNext(&encoder, &((VALUES)[i2]));  \
			munit_assert_int(rc2, ==, 0);                       \
		}                                                           \
	}

/* Decode a response of the given lower/upper case name using the buffer that
 * was written by the gateway. */
#define DECODE(RESPONSE, LOWER)                                     \
	{                                                           \
		int rc2;                                            \
		rc2 = response##LOWER##Decode(f->cursor, RESPONSE); \
		munit_assert_int(rc2, ==, 0);                       \
	}

/* Decode a row with N columns filling the given values. */
#define DECODE_ROW(N, VALUES)                                              \
	{                                                                  \
		struct tupleDecoder decoder;                               \
		int i2;                                                    \
		int rc2;                                                   \
		rc2 = tupleDecoderInit(&decoder, N, f->cursor);            \
		munit_assert_int(rc2, ==, 0);                              \
		for (i2 = 0; i2 < N; i2++) {                               \
			rc2 = tupleDecoderNext(&decoder, &((VALUES)[i2])); \
			munit_assert_int(rc2, ==, 0);                      \
		}                                                          \
	}

/* Handle a request of the given type and check that no error occurs. */
#define HANDLE(TYPE)                                                           \
	{                                                                      \
		int rc2;                                                       \
		f->cursor->p = bufferCursor(f->buf1, 0);                       \
		f->cursor->cap = bufferOffset(f->buf1);                        \
		bufferReset(f->buf2);                                          \
		f->context->invoked = false;                                   \
		f->context->status = -1;                                       \
		f->context->type = -1;                                         \
		rc2 = gatewayHandle(f->gateway, f->handle,                     \
				    DQLITE_REQUEST_##TYPE, f->cursor, f->buf2, \
				    handleCb);                                 \
		munit_assert_int(rc2, ==, 0);                                  \
	}

/* Open a leader connection against the "test" database */
#define OPEN                             \
	{                                \
		struct requestopen open; \
		open.filename = "test";  \
		open.vfs = "";           \
		ENCODE(&open, open);     \
		HANDLE(OPEN);            \
		ASSERT_CALLBACK(0, DB);  \
	}

/* Prepare a statement. The ID will be saved in stmtId. */
#define PREPARE(SQL)                           \
	{                                      \
		struct requestprepare prepare; \
		struct responsestmt stmt;      \
		prepare.dbId = 0;              \
		prepare.sql = SQL;             \
		ENCODE(&prepare, prepare);     \
		HANDLE(PREPARE);               \
		ASSERT_CALLBACK(0, STMT);      \
		DECODE(&stmt, stmt);           \
		stmtId = stmt.id;              \
	}

/* Finalize the statement with the given ID. */
#define FINALIZE(STMT_ID)                        \
	{                                        \
		struct requestfinalize finalize; \
		finalize.dbId = 0;               \
		finalize.stmtId = STMT_ID;       \
		ENCODE(&finalize, finalize);     \
		HANDLE(FINALIZE);                \
		ASSERT_CALLBACK(0, EMPTY);       \
	}

/* Submit a request to execute the given statement. */
#define EXEC_SUBMIT(STMT_ID)             \
	{                                \
		struct requestexec exec; \
		exec.dbId = 0;           \
		exec.stmtId = STMT_ID;   \
		ENCODE(&exec, exec);     \
		HANDLE(EXEC);            \
	}

/* Submit a request to execute the given statement. */
#define EXEC_SQL_SUBMIT(SQL)                   \
	{                                      \
		struct requestexecSql execSql; \
		execSql.dbId = 0;              \
		execSql.sql = SQL;             \
		ENCODE(&execSql, execSql);     \
		HANDLE(EXEC_SQL);              \
	}

/* Wait for the last request to complete */
#define WAIT                                            \
	{                                               \
		unsigned _i;                            \
		for (_i = 0; _i < 60; _i++) {           \
			CLUSTER_STEP;                   \
			if (f->context->invoked) {      \
				break;                  \
			}                               \
		}                                       \
		munit_assert_true(f->context->invoked); \
	}

/* Prepare and exec a statement. */
#define EXEC(SQL)                              \
	{                                      \
		uint64_t _stmtId;              \
		struct requestprepare prepare; \
		struct responsestmt stmt;      \
		prepare.dbId = 0;              \
		prepare.sql = SQL;             \
		ENCODE(&prepare, prepare);     \
		HANDLE(PREPARE);               \
		ASSERT_CALLBACK(0, STMT);      \
		DECODE(&stmt, stmt);           \
		_stmtId = stmt.id;             \
		EXEC_SUBMIT(_stmtId);          \
		WAIT;                          \
		ASSERT_CALLBACK(0, RESULT);    \
		FINALIZE(_stmtId);             \
	}

/* Execute a pragma statement to lowers SQLite's page cache size, in order to
 * force it to write uncommitted dirty pages to the WAL and hance trigger calls
 * to the xFrames hook with non-commit batches. */
#define LOWER_CACHE_SIZE EXEC("PRAGMA cache_size = 1")

/******************************************************************************
 *
 * Assertions.
 *
 ******************************************************************************/

/* Assert that the handle callback has been invoked with the given status and
 * response type. Also, initialize the fixture's cursor to read the response
 * buffer. */
#define ASSERT_CALLBACK(STATUS, UPPER)                                   \
	munit_assert_true(f->context->invoked);                          \
	munit_assert_int(f->context->status, ==, STATUS);                \
	munit_assert_int(f->context->type, ==, DQLITE_RESPONSE_##UPPER); \
	f->cursor->p = bufferCursor(f->buf2, 0);                         \
	f->cursor->cap = bufferOffset(f->buf2);                          \
	bufferReset(f->buf2);                                            \
	f->context->invoked = false;

/* Assert that the failure response generated by the gateway matches the given
 * details. */
#define ASSERT_FAILURE(CODE, MESSAGE)                                \
	{                                                            \
		struct responsefailure failure;                      \
		int rc2;                                             \
		rc2 = responsefailureDecode(f->cursor, &failure);    \
		munit_assert_int(rc2, ==, 0);                        \
		munit_assert_int(failure.code, ==, CODE);            \
		munit_assert_string_equal(failure.message, MESSAGE); \
	}

/******************************************************************************
 *
 * leader
 *
 ******************************************************************************/

struct leaderFixture
{
	FIXTURE;
	struct requestleader request;
	struct responseserver response;
};

TEST_SUITE(leader);
TEST_SETUP(leader)
{
	struct leaderFixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}
TEST_TEAR_DOWN(leader)
{
	struct leaderFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* If the leader is not available, an empty string is returned. */
TEST_CASE(leader, notAvailable, NULL)
{
	struct leaderFixture *f = data;
	(void)params;
	ENCODE(&f->request, leader);
	HANDLE(LEADER);
	ASSERT_CALLBACK(0, SERVER);
	DECODE(&f->response, server);
	munit_assert_int(f->response.id, ==, 0);
	munit_assert_string_equal(f->response.address, "");
	return MUNIT_OK;
}

/* The leader is the same node serving the request. */
TEST_CASE(leader, sameNode, NULL)
{
	struct leaderFixture *f = data;
	(void)params;
	CLUSTER_ELECT(0);
	ENCODE(&f->request, leader);
	HANDLE(LEADER);
	ASSERT_CALLBACK(0, SERVER);
	DECODE(&f->response, server);
	munit_assert_string_equal(f->response.address, "1");
	return MUNIT_OK;
}

/* The leader is a different node than the one serving the request. */
TEST_CASE(leader, otherNode, NULL)
{
	struct leaderFixture *f = data;
	(void)params;
	CLUSTER_ELECT(1);
	ENCODE(&f->request, leader);
	HANDLE(LEADER);
	ASSERT_CALLBACK(0, SERVER);
	DECODE(&f->response, server);
	munit_assert_string_equal(f->response.address, "2");
	return MUNIT_OK;
}

/******************************************************************************
 *
 * open
 *
 ******************************************************************************/

struct openFixture
{
	FIXTURE;
	struct requestopen request;
	struct responsedb response;
};

TEST_SUITE(open);
TEST_SETUP(open)
{
	struct openFixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}
TEST_TEAR_DOWN(open)
{
	struct openFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Successfully open a database connection. */
TEST_CASE(open, success, NULL)
{
	struct openFixture *f = data;
	(void)params;
	f->request.filename = "test";
	f->request.vfs = "";
	ENCODE(&f->request, open);
	HANDLE(OPEN);
	ASSERT_CALLBACK(0, DB);
	DECODE(&f->response, db);
	munit_assert_int(f->response.id, ==, 0);
	return MUNIT_OK;
}

TEST_GROUP(open, error);

/* Attempting to open two databases on the same gateway results in an error. */
TEST_CASE(open, error, twice, NULL)
{
	struct openFixture *f = data;
	(void)params;
	f->request.filename = "test";
	f->request.vfs = "";
	ENCODE(&f->request, open);
	HANDLE(OPEN);
	ASSERT_CALLBACK(0, DB);
	ENCODE(&f->request, open);
	HANDLE(OPEN);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_BUSY,
		       "a database for this connection is already open");
	return MUNIT_OK;
}

/******************************************************************************
 *
 * prepare
 *
 ******************************************************************************/

struct prepareFixture
{
	FIXTURE;
	struct requestprepare request;
	struct responsestmt response;
};

TEST_SUITE(prepare);
TEST_SETUP(prepare)
{
	struct prepareFixture *f = munit_malloc(sizeof *f);
	SETUP;
	OPEN;
	return f;
}
TEST_TEAR_DOWN(prepare)
{
	struct prepareFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Successfully prepare a statement. */
TEST_CASE(prepare, success, NULL)
{
	struct prepareFixture *f = data;
	(void)params;
	f->request.dbId = 0;
	f->request.sql = "CREATE TABLE test (n INT)";
	ENCODE(&f->request, prepare);
	HANDLE(PREPARE);
	ASSERT_CALLBACK(0, STMT);
	DECODE(&f->response, stmt);
	munit_assert_int(f->response.id, ==, 0);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * exec
 *
 ******************************************************************************/

struct execFixture
{
	FIXTURE;
	struct requestexec request;
	struct responseresult response;
};

TEST_SUITE(exec);
TEST_SETUP(exec)
{
	struct execFixture *f = munit_malloc(sizeof *f);
	SETUP;
	OPEN;
	return f;
}
TEST_TEAR_DOWN(exec)
{
	struct execFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Successfully execute a simple statement with no parameters. */
TEST_CASE(exec, simple, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	(void)params;
	CLUSTER_ELECT(0);
	PREPARE("CREATE TABLE test (n INT)");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, exec);
	HANDLE(EXEC);
	CLUSTER_APPLIED(2);
	ASSERT_CALLBACK(0, RESULT);
	DECODE(&f->response, result);
	munit_assert_int(f->response.lastInsertId, ==, 0);
	munit_assert_int(f->response.rowsAffected, ==, 0);
	return MUNIT_OK;
}

/* Successfully execute a statement with a one parameter. */
TEST_CASE(exec, oneParam, NULL)
{
	struct execFixture *f = data;
	struct value value;
	uint64_t stmtId;
	(void)params;
	CLUSTER_ELECT(0);

	/* Create the test table */
	EXEC("CREATE TABLE test (n INT)");

	/* Insert a row with one parameter */
	PREPARE("INSERT INTO test VALUES (?)");
	f->request.stmtId = stmtId;
	ENCODE(&f->request, exec);
	value.type = SQLITE_INTEGER;
	value.integer = 7;
	ENCODE_PARAMS(1, &value);
	HANDLE(EXEC);
	CLUSTER_APPLIED(3);
	ASSERT_CALLBACK(0, RESULT);
	DECODE(&f->response, result);
	munit_assert_int(f->response.lastInsertId, ==, 1);
	munit_assert_int(f->response.rowsAffected, ==, 1);

	return MUNIT_OK;
}

/* Successfully execute a statement with a blob parameter. */
TEST_CASE(exec, blob, NULL)
{
	struct execFixture *f = data;
	struct requestquery query;
	struct value value;
	char buf[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
	uint64_t stmtId;
	uint64_t n;
	const char *column;
	(void)params;
	CLUSTER_ELECT(0);

	/* Create the test table */
	EXEC("CREATE TABLE test (data BLOB)");

	/* Insert a row with one parameter */
	PREPARE("INSERT INTO test VALUES (?)");
	f->request.stmtId = stmtId;
	ENCODE(&f->request, exec);
	value.type = SQLITE_BLOB;
	value.blob.base = buf;
	value.blob.len = sizeof buf;
	ENCODE_PARAMS(1, &value);
	HANDLE(EXEC);
	CLUSTER_APPLIED(3);
	ASSERT_CALLBACK(0, RESULT);
	DECODE(&f->response, result);
	munit_assert_int(f->response.lastInsertId, ==, 1);
	munit_assert_int(f->response.rowsAffected, ==, 1);

	PREPARE("SELECT data FROM test");
	query.dbId = 0;
	query.stmtId = stmtId;
	ENCODE(&query, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);

	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "data");
	DECODE_ROW(1, &value);
	munit_assert_int(value.type, ==, SQLITE_BLOB);
	munit_assert_int(value.blob.len, ==, sizeof buf);
	munit_assert_int(value.blob.base[0], ==, 'a');
	munit_assert_int(value.blob.base[7], ==, 'h');

	return MUNIT_OK;
}

/* The server is not the leader anymore when the first frames hook for a
 * non-commit frames batch fires. The same leader gets re-elected. */
TEST_CASE(exec, framesNotLeader_1stNonCommitReElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 162; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a page cache flush to the WAL, which fails because we are not
	 * leader anymore */
	PREPARE("INSERT INTO test(n) VALUES(1)");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_NOT_LEADER, "not leader");

	/* Re-elect ourselves and re-try */
	CLUSTER_ELECT(0);
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The server is not the leader anymore when the first frames hook for a
 * non-commit frames batch fires. Another leader gets re-elected. */
TEST_CASE(exec, framesNotLeader_1stNonCommitOtherElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 162; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a page cache flush to the WAL, which fails because we are not
	 * leader anymore */
	PREPARE("INSERT INTO test(n) VALUES(1)");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_NOT_LEADER, "not leader");

	/* Elect another leader and re-try */
	CLUSTER_ELECT(1);
	SELECT(1);
	OPEN;
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The server is not the leader anymore when the second frames hook for a
 * non-commit frames batch fires. The same leader gets re-elected. */
TEST_CASE(exec, framesNotLeader_2ndNonCommitReElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache a first time,
	 * flush it and then fill it a second time. */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 234; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a second page cache flush to the WAL, which fails because we
	 * are not leader anymore */
	PREPARE("INSERT INTO test(n) VALUES(1)");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_NOT_LEADER, "not leader");

	/* Re-elect ourselves and re-try */
	CLUSTER_ELECT(0);
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The gateway is closed while a raft commit is in flight. */
TEST_CASE(exec, closeWhileInFlight, NULL)
{
	struct execFixture *f = data;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache and trigger
	 * an apply request. */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 162; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a second page cache flush to the WAL, and abort before it's
	 * done. */
	EXEC_SQL_SUBMIT("INSERT INTO test(n) VALUES(1)");
	return MUNIT_OK;
}

/* The server is not the leader anymore when the second frames hook for a
 * non-commit frames batch fires. Another leader gets elected. */
TEST_CASE(exec, framesNotLeader_2ndNonCommitOtherElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache a first time,
	 * flush it and then fill it a second time. */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 234; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a second page cache flush to the WAL, which fails because we
	 * are not leader anymore */
	PREPARE("INSERT INTO test(n) VALUES(1)");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);

	/* Elect another leader and re-try */
	CLUSTER_ELECT(1);
	SELECT(1);
	OPEN;
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The server loses leadership after trying to apply the first Frames command
 * for a non-commit frames batch. The same leader gets re-elected. */
TEST_CASE(exec, framesLeadershipLost_1stNonCommitReElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 162; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trigger a page cache flush to the WAL */
	EXEC("INSERT INTO test(n) VALUES(1)");

	/* Try to commit */
	PREPARE("COMMIT");
	EXEC_SUBMIT(stmtId);
	CLUSTER_DEPOSE;
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_LEADERSHIP_LOST, "disk I/O error");

	/* Re-elect ourselves and re-try */
	CLUSTER_ELECT(0);
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The server is not the leader anymore when the undo hook for a writing
 * transaction fires. The same leader gets re-elected. */
TEST_CASE(exec, undoNotLeaderPendingReElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache a first time */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 163; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trying to rollback fails because we are not leader anymore */
	PREPARE("ROLLBACK");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_NOT_LEADER, "not leader");

	/* Re-elect ourselves and re-try */
	CLUSTER_ELECT(0);
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* The server is not the leader anymore when the undo hook for a writing
 * transaction fires. Another leader gets elected. */
TEST_CASE(exec, undoNotLeaderPendingOtherElected, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	unsigned i;
	(void)params;
	CLUSTER_ELECT(0);

	/* Accumulate enough dirty data to fill the page cache a first time */
	LOWER_CACHE_SIZE;
	EXEC("CREATE TABLE test (n INT)");
	EXEC("BEGIN");
	for (i = 0; i < 163; i++) {
		EXEC("INSERT INTO test(n) VALUES(1)");
	}

	/* Trying to rollback fails because we are not leader anymore */
	PREPARE("ROLLBACK");
	CLUSTER_DEPOSE;
	EXEC_SUBMIT(stmtId);
	ASSERT_CALLBACK(0, FAILURE);
	ASSERT_FAILURE(SQLITE_IOERR_NOT_LEADER, "not leader");

	/* Re-elect ourselves and re-try */
	CLUSTER_ELECT(1);
	SELECT(1);
	OPEN;
	EXEC("INSERT INTO test(n) VALUES(1)");

	return MUNIT_OK;
}

/* A follower remains behind and needs to restore state from a snapshot. */
TEST_CASE(exec, restore, NULL)
{
	struct execFixture *f = data;
	uint64_t stmtId;
	struct requestquery request;
	struct responserows response;
	struct value value;
	uint64_t n;
	const char *column;
	(void)params;
	CLUSTER_SNAPSHOT_THRESHOLD(0, 5);
	CLUSTER_SNAPSHOT_TRAILING(0, 2);
	CLUSTER_ELECT(0);
	CLUSTER_DISCONNECT(0, 1);
	EXEC("CREATE TABLE test (n INT)");
	EXEC("INSERT INTO test(n) VALUES(1)");
	EXEC("INSERT INTO test(n) VALUES(2)");
	CLUSTER_RECONNECT(0, 1);
	CLUSTER_APPLIED(4);

	/* TODO: the query below fails because we can exec queries only against
	 * the leader. */
	return MUNIT_SKIP;

	/* The follower contains the expected rows. */
	SELECT(1);
	OPEN;
	PREPARE("SELECT n FROM test");
	request.dbId = 0;
	request.stmtId = stmtId;
	ENCODE(&request, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);
	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");
	DECODE_ROW(1, &value);
	munit_assert_int(value.type, ==, SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 1);
	DECODE_ROW(1, &value);
	munit_assert_int(value.type, ==, SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 2);
	DECODE(&response, rows);
	munit_assert_ulong(response.eof, ==, DQLITE_RESPONSE_ROWS_DONE);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * query
 *
 ******************************************************************************/

struct queryFixture
{
	FIXTURE;
	struct requestquery request;
	struct responserows response;
};

TEST_SUITE(query);
TEST_SETUP(query)
{
	struct queryFixture *f = munit_malloc(sizeof *f);
	SETUP;
	OPEN;
	CLUSTER_ELECT(0);
	EXEC("CREATE TABLE test (n INT, data BLOB)");
	return f;
}
TEST_TEAR_DOWN(query)
{
	struct queryFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Successfully query a simple statement with no parameters and yielding no
 * rows. */
TEST_CASE(query, simple, NULL)
{
	struct queryFixture *f = data;
	uint64_t stmtId;
	uint64_t n;
	const char *column;
	(void)params;
	PREPARE("SELECT n FROM test");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);
	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");
	DECODE(&f->response, rows);
	munit_assert_ulong(f->response.eof, ==, DQLITE_RESPONSE_ROWS_DONE);

	return MUNIT_OK;
}

/* Successfully query a simple statement with no parameters yielding one row. */
TEST_CASE(query, oneRow, NULL)
{
	struct queryFixture *f = data;
	uint64_t stmtId;
	uint64_t n;
	const char *column;
	struct value value;
	(void)params;
	EXEC("INSERT INTO test(n) VALUES(666)");

	PREPARE("SELECT n FROM test");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);

	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");
	DECODE_ROW(1, &value);
	munit_assert_int(value.type, ==, SQLITE_INTEGER);
	munit_assert_int(value.integer, ==, 666);
	DECODE(&f->response, rows);
	munit_assert_ulong(f->response.eof, ==, DQLITE_RESPONSE_ROWS_DONE);

	return MUNIT_OK;
}

/* Successfully query that yields a large number of rows that need to be split
 * into several reponses. */
TEST_CASE(query, large, NULL)
{
	struct queryFixture *f = data;
	unsigned i;
	uint64_t stmtId;
	uint64_t n;
	const char *column;
	struct value value;
	bool finished;
	(void)params;
	EXEC("BEGIN");
	for (i = 0; i < 500; i++) {
		EXEC("INSERT INTO test(n) VALUES(123)");
	}
	EXEC("COMMIT");

	PREPARE("SELECT n FROM test");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);

	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");

	for (i = 0; i < 255; i++) {
		DECODE_ROW(1, &value);
		munit_assert_int(value.type, ==, SQLITE_INTEGER);
		munit_assert_int(value.integer, ==, 123);
	}

	DECODE(&f->response, rows);
	munit_assert_ulong(f->response.eof, ==, DQLITE_RESPONSE_ROWS_PART);

	gatewayResume(f->gateway, &finished);
	munit_assert_false(finished);

	ASSERT_CALLBACK(0, ROWS);

	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");

	for (i = 0; i < 245; i++) {
		DECODE_ROW(1, &value);
		munit_assert_int(value.type, ==, SQLITE_INTEGER);
		munit_assert_int(value.integer, ==, 123);
	}

	DECODE(&f->response, rows);
	munit_assert_ulong(f->response.eof, ==, DQLITE_RESPONSE_ROWS_DONE);

	return MUNIT_OK;
}

/* Perform a query using a prepared statement with parameters */
TEST_CASE(query, params, NULL)
{
	struct queryFixture *f = data;
	struct value values[2];
	uint64_t stmtId;
	(void)params;
	EXEC("BEGIN");
	EXEC("INSERT INTO test(n) VALUES(1)");
	EXEC("INSERT INTO test(n) VALUES(2)");
	EXEC("INSERT INTO test(n) VALUES(3)");
	EXEC("INSERT INTO test(n) VALUES(4)");
	EXEC("COMMIT");

	PREPARE("SELECT n FROM test WHERE n > ? AND n < ?");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;

	ENCODE(&f->request, query);
	values[0].type = SQLITE_INTEGER;
	values[0].integer = 1;
	values[1].type = SQLITE_INTEGER;
	values[1].integer = 4;
	ENCODE_PARAMS(2, values);

	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);
	return MUNIT_OK;
}

/* Interrupt a large query. */
TEST_CASE(query, interrupt, NULL)
{
	struct queryFixture *f = data;
	struct requestinterrupt interrupt;
	unsigned i;
	uint64_t stmtId;
	uint64_t n;
	const char *column;
	struct value value;
	(void)params;
	EXEC("BEGIN");
	for (i = 0; i < 500; i++) {
		EXEC("INSERT INTO test(n) VALUES(123)");
	}
	EXEC("COMMIT");

	PREPARE("SELECT n FROM test");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, query);
	HANDLE(QUERY);
	ASSERT_CALLBACK(0, ROWS);

	uint64Decode(f->cursor, &n);
	munit_assert_int(n, ==, 1);
	textDecode(f->cursor, &column);
	munit_assert_string_equal(column, "n");

	for (i = 0; i < 255; i++) {
		DECODE_ROW(1, &value);
		munit_assert_int(value.type, ==, SQLITE_INTEGER);
		munit_assert_int(value.integer, ==, 123);
	}

	DECODE(&f->response, rows);
	munit_assert_ulong(f->response.eof, ==, DQLITE_RESPONSE_ROWS_PART);

	ENCODE(&interrupt, interrupt);
	HANDLE(INTERRUPT);

	ASSERT_CALLBACK(0, EMPTY);

	return MUNIT_OK;
}

/* Submit a query request right after the server has been re-elected and needs
 * to catch up with logs. */
TEST_CASE(query, barrier, NULL)
{
	struct queryFixture *f = data;
	uint64_t stmtId;
	(void)params;

	PREPARE("INSERT INTO test(n) VALUES(1)");
	EXEC_SUBMIT(stmtId);
	CLUSTER_DEPOSE;
	ASSERT_CALLBACK(0, FAILURE);

	/* Re-elect ourselves and issue a query request */
	CLUSTER_ELECT(0);

	PREPARE("SELECT n FROM test");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, query);
	HANDLE(QUERY);
	WAIT;
	ASSERT_CALLBACK(0, ROWS);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * finalize
 *
 ******************************************************************************/

struct finalizeFixture
{
	FIXTURE;
	struct requestfinalize request;
	struct responseempty response;
};

TEST_SUITE(finalize);
TEST_SETUP(finalize)
{
	struct finalizeFixture *f = munit_malloc(sizeof *f);
	SETUP;
	OPEN;
	return f;
}
TEST_TEAR_DOWN(finalize)
{
	struct finalizeFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Finalize a prepared statement. */
TEST_CASE(finalize, success, NULL)
{
	uint64_t stmtId;
	struct finalizeFixture *f = data;
	(void)params;
	PREPARE("CREATE TABLE test (n INT)");
	f->request.dbId = 0;
	f->request.stmtId = stmtId;
	ENCODE(&f->request, finalize);
	HANDLE(FINALIZE);
	ASSERT_CALLBACK(0, EMPTY);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * execSql
 *
 ******************************************************************************/

struct execSqlFixture
{
	FIXTURE;
	struct requestexecSql request;
	struct responseresult response;
};

TEST_SUITE(execSql);
TEST_SETUP(execSql)
{
	struct execSqlFixture *f = munit_malloc(sizeof *f);
	SETUP;
	CLUSTER_ELECT(0);
	OPEN;
	return f;
}
TEST_TEAR_DOWN(execSql)
{
	struct execSqlFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Exec a SQL text with a single query. */
TEST_CASE(execSql, single, NULL)
{
	struct execSqlFixture *f = data;
	(void)params;
	f->request.dbId = 0;
	f->request.sql = "CREATE TABLE test (n INT)";
	ENCODE(&f->request, execSql);
	HANDLE(EXEC_SQL);
	CLUSTER_APPLIED(2);
	ASSERT_CALLBACK(0, RESULT);
	return MUNIT_OK;
}

/* Exec a SQL text with a multiple queries. */
TEST_CASE(execSql, multi, NULL)
{
	struct execSqlFixture *f = data;
	(void)params;
	f->request.dbId = 0;
	f->request.sql =
	    "CREATE TABLE test (n INT); INSERT INTO test VALUES(1)";
	ENCODE(&f->request, execSql);
	HANDLE(EXEC_SQL);
	WAIT;
	ASSERT_CALLBACK(0, RESULT);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * querySql
 *
 ******************************************************************************/

struct querySqlFixture
{
	FIXTURE;
	struct requestquerySql request;
	struct responserows response;
};

TEST_SUITE(querySql);
TEST_SETUP(querySql)
{
	struct querySqlFixture *f = munit_malloc(sizeof *f);
	SETUP;
	CLUSTER_ELECT(0);
	OPEN;
	EXEC("CREATE TABLE test (n INT)");
	return f;
}
TEST_TEAR_DOWN(querySql)
{
	struct querySqlFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Exec a SQL query whose result set fits in a page. */
TEST_CASE(querySql, small, NULL)
{
	struct querySqlFixture *f = data;
	(void)params;
	EXEC("INSERT INTO test VALUES(123)");
	f->request.dbId = 0;
	f->request.sql = "SELECT n FROM test";
	ENCODE(&f->request, querySql);
	HANDLE(QUERY_SQL);
	ASSERT_CALLBACK(0, ROWS);
	return MUNIT_OK;
}

/* Perform a query with parameters */
TEST_CASE(querySql, params, NULL)
{
	struct querySqlFixture *f = data;
	struct value values[2];
	(void)params;
	EXEC("BEGIN");
	EXEC("INSERT INTO test(n) VALUES(1)");
	EXEC("INSERT INTO test(n) VALUES(2)");
	EXEC("INSERT INTO test(n) VALUES(3)");
	EXEC("INSERT INTO test(n) VALUES(4)");
	EXEC("COMMIT");

	f->request.dbId = 0;
	f->request.sql = "SELECT n FROM test WHERE n > ? AND n < ?";
	ENCODE(&f->request, querySql);
	values[0].type = SQLITE_INTEGER;
	values[0].integer = 1;
	values[1].type = SQLITE_INTEGER;
	values[1].integer = 4;
	ENCODE_PARAMS(2, values);

	HANDLE(QUERY_SQL);
	ASSERT_CALLBACK(0, ROWS);
	return MUNIT_OK;
}
