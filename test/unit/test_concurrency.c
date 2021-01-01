#include "../lib/cluster.h"
#include "../lib/runner.h"

#include "../../src/gateway.h"
#include "../../src/protocol.h"
#include "../../src/request.h"
#include "../../src/response.h"

TEST_MODULE(concurrency);

/******************************************************************************
 *
 * Fixture.
 *
 ******************************************************************************/

#define N_GATEWAYS 2

/* Context for a gateway handle request */
struct context
{
	bool invoked;
	int status;
	int type;
};

/* Standalone leader database connection */
struct connection
{
	struct gateway gateway;
	struct buffer request;  /* Request payload */
	struct buffer response; /* Response payload */
	struct handle handle;   /* Async handle request */
	struct context context;
};

#define FIXTURE          \
	FIXTURE_CLUSTER; \
	struct connection connections[N_GATEWAYS]

#define SETUP                                                      \
	unsigned i;                                                \
	int rc;                                                    \
	SETUP_CLUSTER(V2);                                         \
	CLUSTER_ELECT(0);                                          \
	for (i = 0; i < N_GATEWAYS; i++) {                         \
		struct connection *c = &f->connections[i];         \
		struct requestopen open;                           \
		struct responsedb db;                              \
		gatewayInit(&c->gateway, CLUSTER_CONFIG(0),        \
			    CLUSTER_REGISTRY(0), CLUSTER_RAFT(0)); \
		c->handle.data = &c->context;                      \
		rc = bufferInit(&c->request);                      \
		munit_assert_int(rc, ==, 0);                       \
		rc = bufferInit(&c->response);                     \
		munit_assert_int(rc, ==, 0);                       \
		open.filename = "test";                            \
		open.vfs = "";                                     \
		ENCODE(c, &open, open);                            \
		HANDLE(c, OPEN);                                   \
		ASSERT_CALLBACK(c, 0, DB);                         \
		DECODE(c, &db, db);                                \
		munit_assert_int(db.id, ==, 0);                    \
	}

#define TEAR_DOWN                                          \
	unsigned i;                                        \
	for (i = 0; i < N_GATEWAYS; i++) {                 \
		struct connection *c = &f->connections[i]; \
		bufferClose(&c->request);                  \
		bufferClose(&c->response);                 \
		gatewayClose(&c->gateway);                 \
	}                                                  \
	TEAR_DOWN_CLUSTER;

static void fixtureHandleCb(struct handle *req, int status, int type)
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

/* Reset the request buffer of the given connection and encode a request of the
 * given lower case name. */
#define ENCODE(C, REQUEST, LOWER)                            \
	{                                                    \
		size_t n2 = request##LOWER##Sizeof(REQUEST); \
		void *cursor;                                \
		bufferReset(&C->request);                    \
		cursor = bufferAdvance(&C->request, n2);     \
		munit_assert_ptr_not_null(cursor);           \
		request##LOWER##Encode(REQUEST, &cursor);    \
	}

/* Decode a response of the given lower/upper case name using the response
 * buffer of the given connection. */
#define DECODE(C, RESPONSE, LOWER)                                \
	{                                                         \
		struct cursor cursor;                             \
		int rc2;                                          \
		cursor.p = bufferCursor(&C->response, 0);         \
		cursor.cap = bufferOffset(&C->response);          \
		rc2 = response##LOWER##Decode(&cursor, RESPONSE); \
		munit_assert_int(rc2, ==, 0);                     \
	}

/* Submit a request of the given type to the given connection and check that no
 * error occurs. */
#define HANDLE(C, TYPE)                                             \
	{                                                           \
		struct cursor cursor;                               \
		int rc2;                                            \
		cursor.p = bufferCursor(&C->request, 0);            \
		cursor.cap = bufferOffset(&C->request);             \
		bufferReset(&C->response);                          \
		rc2 = gatewayHandle(&C->gateway, &C->handle,        \
				    DQLITE_REQUEST_##TYPE, &cursor, \
				    &C->response, fixtureHandleCb); \
		munit_assert_int(rc2, ==, 0);                       \
	}

/* Prepare a statement on the given connection. The ID will be saved in
 * the STMT_ID pointer. */
#define PREPARE(C, SQL, STMT_ID)               \
	{                                      \
		struct requestprepare prepare; \
		struct responsestmt stmt;      \
		prepare.dbId = 0;              \
		prepare.sql = SQL;             \
		ENCODE(C, &prepare, prepare);  \
		HANDLE(C, PREPARE);            \
		ASSERT_CALLBACK(C, 0, STMT);   \
		DECODE(C, &stmt, stmt);        \
		*(STMT_ID) = stmt.id;          \
	}

/* Submit a request to exec a statement. */
#define EXEC(C, STMT_ID)                 \
	{                                \
		struct requestexec exec; \
		exec.dbId = 0;           \
		exec.stmtId = STMT_ID;   \
		ENCODE(C, &exec, exec);  \
		HANDLE(C, EXEC);         \
	}

/* Submit a query request. */
#define QUERY(C, STMT_ID)                  \
	{                                  \
		struct requestquery query; \
		query.dbId = 0;            \
		query.stmtId = STMT_ID;    \
		ENCODE(C, &query, query);  \
		HANDLE(C, QUERY);          \
	}

/* Wait for the gateway of the given connection to finish handling a request. */
#define WAIT(C)                                        \
	{                                              \
		unsigned _i;                           \
		for (_i = 0; _i < 50; _i++) {          \
			CLUSTER_STEP;                  \
			if (C->context.invoked) {      \
				break;                 \
			}                              \
		}                                      \
		munit_assert_true(C->context.invoked); \
	}

/******************************************************************************
 *
 * Assertions.
 *
 ******************************************************************************/

/* Assert that the handle callback of the given connection has been invoked with
 * the given status and response type.. */
#define ASSERT_CALLBACK(C, STATUS, UPPER)                               \
	munit_assert_true(C->context.invoked);                          \
	munit_assert_int(C->context.status, ==, STATUS);                \
	munit_assert_int(C->context.type, ==, DQLITE_RESPONSE_##UPPER); \
	C->context.invoked = false

/* Assert that the failure response generated by the gateway of the given
 * connection matches the given details. */
#define ASSERT_FAILURE(C, CODE, MESSAGE)                             \
	{                                                            \
		struct responsefailure failure;                      \
		DECODE(C, &failure, failure);                        \
		munit_assert_int(failure.code, ==, CODE);            \
		munit_assert_string_equal(failure.message, MESSAGE); \
	}

/******************************************************************************
 *
 * Concurrent exec requests
 *
 ******************************************************************************/

struct execFixture
{
	FIXTURE;
	struct connection *c1;
	struct connection *c2;
	unsigned stmtId1;
	unsigned stmtId2;
};

TEST_SUITE(exec);
TEST_SETUP(exec)
{
	struct execFixture *f = munit_malloc(sizeof *f);
	SETUP;
	f->c1 = &f->connections[0];
	f->c2 = &f->connections[1];
	return f;
}
TEST_TEAR_DOWN(exec)
{
	struct execFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* If another leader connection has submitted an Open request and is waiting for
 * it to complete, SQLITE_BUSY is returned. */
TEST_CASE(exec, open, NULL)
{
	struct execFixture *f = data;
	(void)params;

	PREPARE(f->c1, "CREATE TABLE test1 (n INT)", &f->stmtId1);
	PREPARE(f->c2, "CREATE TABLE test2 (n INT)", &f->stmtId2);

	EXEC(f->c1, f->stmtId1);
	EXEC(f->c2, f->stmtId2);
	WAIT(f->c2);
	ASSERT_CALLBACK(f->c2, 0, FAILURE);
	ASSERT_FAILURE(f->c2, SQLITE_BUSY, "database is locked");
	WAIT(f->c1);
	ASSERT_CALLBACK(f->c1, 0, RESULT);

	return MUNIT_OK;
}

/* If an exec request is already in progress on another leader connection,
 * SQLITE_BUSY is returned. */
TEST_CASE(exec, tx, NULL)
{
	struct execFixture *f = data;
	(void)params;

	/* Create a test table using connection 0 */
	PREPARE(f->c1, "CREATE TABLE test (n INT)", &f->stmtId1);
	EXEC(f->c1, f->stmtId1);
	WAIT(f->c1);
	ASSERT_CALLBACK(f->c1, 0, RESULT);
	PREPARE(f->c1, "INSERT INTO test(n) VALUES(1)", &f->stmtId1);
	PREPARE(f->c2, "INSERT INTO test(n) VALUES(1)", &f->stmtId2);

	EXEC(f->c1, f->stmtId1);
	EXEC(f->c2, f->stmtId2);
	WAIT(f->c2);
	ASSERT_CALLBACK(f->c2, 0, FAILURE);
	ASSERT_FAILURE(f->c2, SQLITE_BUSY, "database is locked");
	WAIT(f->c1);
	ASSERT_CALLBACK(f->c1, 0, RESULT);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * Concurrent query requests
 *
 ******************************************************************************/

struct queryFixture
{
	FIXTURE;
	struct connection *c1;
	struct connection *c2;
	unsigned stmtId1;
	unsigned stmtId2;
};

TEST_SUITE(query);
TEST_SETUP(query)
{
	struct execFixture *f = munit_malloc(sizeof *f);
	SETUP;
	f->c1 = &f->connections[0];
	f->c2 = &f->connections[1];
	PREPARE(f->c1, "CREATE TABLE test (n INT)", &f->stmtId1);
	EXEC(f->c1, f->stmtId1);
	WAIT(f->c1);
	return f;
}
TEST_TEAR_DOWN(query)
{
	struct execFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* Handle a query request while there is a transaction in progress. */
TEST_CASE(query, tx, NULL)
{
	struct execFixture *f = data;
	(void)params;
	PREPARE(f->c1, "INSERT INTO test VALUES(1)", &f->stmtId1);
	PREPARE(f->c2, "SELECT n FROM test", &f->stmtId2);
	EXEC(f->c1, f->stmtId1);
	QUERY(f->c2, f->stmtId2);
	WAIT(f->c1);
	WAIT(f->c2);
	ASSERT_CALLBACK(f->c1, 0, RESULT);
	ASSERT_CALLBACK(f->c2, 0, ROWS);
	return MUNIT_OK;
}
