#include <raft.h>
#include <raft/uv.h>

#include "../../include/dqlite.h"

#include "../lib/client.h"
#include "../lib/config.h"
#include "../lib/heap.h"
#include "../lib/logger.h"
#include "../lib/raft.h"
#include "../lib/registry.h"
#include "../lib/runner.h"
#include "../lib/sqlite.h"
#include "../lib/vfs.h"

#include "../../src/client.h"
#include "../../src/conn.h"
#include "../../src/gateway.h"
#include "../../src/lib/transport.h"
#include "../../src/transport.h"

TEST_MODULE(conn);

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

static void connCloseCb(struct conn *conn)
{
	bool *closed = conn->queue[0];
	*closed = true;
}

#define FIXTURE              \
	FIXTURE_LOGGER;      \
	FIXTURE_VFS;         \
	FIXTURE_CONFIG;      \
	FIXTURE_REGISTRY;    \
	FIXTURE_RAFT;        \
	FIXTURE_CLIENT;      \
	struct conn conn;    \
	bool closed;

#define SETUP                                                                  \
	struct uv_stream_s *stream;                                            \
	int rv;                                                                \
	SETUP_HEAP;                                                            \
	SETUP_SQLITE;                                                          \
	SETUP_LOGGER;                                                          \
	SETUP_VFS;                                                             \
	SETUP_CONFIG;                                                          \
	SETUP_REGISTRY;                                                        \
	SETUP_RAFT;                                                            \
	SETUP_CLIENT;                                                          \
	RAFT_BOOTSTRAP;                                                        \
	RAFT_START;                                                            \
	rv = transportStream(&f->loop, f->server, &stream);                    \
	munit_assert_int(rv, ==, 0);                                           \
	f->closed = false;                                                     \
	f->conn.queue[0] = &f->closed;                                         \
	rv = connStart(&f->conn, &f->config, &f->loop, &f->registry, &f->raft, \
		       stream, &f->raftTransport, connCloseCb);                \
	munit_assert_int(rv, ==, 0)

#define TEAR_DOWN                       \
	connStop(&f->conn);             \
	while (!f->closed) {            \
		testUvRun(&f->loop, 1); \
	};                              \
	TEAR_DOWN_RAFT;                 \
	TEAR_DOWN_CLIENT;               \
	TEAR_DOWN_REGISTRY;             \
	TEAR_DOWN_CONFIG;               \
	TEAR_DOWN_VFS;                  \
	TEAR_DOWN_LOGGER;               \
	TEAR_DOWN_SQLITE;               \
	TEAR_DOWN_HEAP

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Send the initial client handshake. */
#define HANDSHAKE                                      \
	{                                              \
		int rv2;                               \
		rv2 = clientSendHandshake(&f->client); \
		munit_assert_int(rv2, ==, 0);          \
		testUvRun(&f->loop, 1);                \
	}

/* Open a test database. */
#define OPEN                                              \
	{                                                 \
		int rv2;                                  \
		rv2 = clientSendOpen(&f->client, "test"); \
		munit_assert_int(rv2, ==, 0);             \
		testUvRun(&f->loop, 2);                   \
		rv2 = clientRecvDb(&f->client);           \
		munit_assert_int(rv2, ==, 0);             \
	}

/* Prepare a statement. */
#define PREPARE(SQL, STMT_ID)                              \
	{                                                  \
		int rv2;                                   \
		rv2 = clientSendPrepare(&f->client, SQL);  \
		munit_assert_int(rv2, ==, 0);              \
		testUvRun(&f->loop, 1);                    \
		rv2 = clientRecvStmt(&f->client, STMT_ID); \
		munit_assert_int(rv2, ==, 0);              \
	}

/* Execute a statement. */
#define EXEC(STMT_ID, LAST_INSERT_ID, ROWS_AFFECTED, LOOP)         \
	{                                                          \
		int rv2;                                           \
		rv2 = clientSendExec(&f->client, STMT_ID);         \
		munit_assert_int(rv2, ==, 0);                      \
		testUvRun(&f->loop, LOOP);                         \
		rv2 = clientRecvResult(&f->client, LAST_INSERT_ID, \
				       ROWS_AFFECTED);             \
		munit_assert_int(rv2, ==, 0);                      \
	}

/* Execute a non-prepared statement. */
#define EXEC_SQL(SQL, LAST_INSERT_ID, ROWS_AFFECTED, LOOP)         \
	{                                                          \
		int rv2;                                           \
		rv2 = clientSendExecSQL(&f->client, SQL);          \
		munit_assert_int(rv2, ==, 0);                      \
		testUvRun(&f->loop, LOOP);                         \
		rv2 = clientRecvResult(&f->client, LAST_INSERT_ID, \
				       ROWS_AFFECTED);             \
		munit_assert_int(rv2, ==, 0);                      \
	}

/* Perform a query. */
#define QUERY(STMT_ID, ROWS)                                \
	{                                                   \
		int rv2;                                    \
		rv2 = clientSendQuery(&f->client, STMT_ID); \
		munit_assert_int(rv2, ==, 0);               \
		testUvRun(&f->loop, 2);                     \
		rv2 = clientRecvRows(&f->client, ROWS);     \
		munit_assert_int(rv2, ==, 0);               \
	}

/******************************************************************************
 *
 * Handle the handshake
 *
 ******************************************************************************/

TEST_SUITE(handshake);

struct handshakeFixture
{
	FIXTURE;
};

TEST_SETUP(handshake)
{
	struct handshakeFixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}

TEST_TEAR_DOWN(handshake)
{
	struct handshakeFixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST_CASE(handshake, success, NULL)
{
	struct handshakeFixture *f = data;
	(void)params;
	HANDSHAKE;
	return MUNIT_OK;
}

/******************************************************************************
 *
 * Handle an open request
 *
 ******************************************************************************/

TEST_SUITE(open);

struct openFixture
{
	FIXTURE;
};

TEST_SETUP(open)
{
	struct openFixture *f = munit_malloc(sizeof *f);
	SETUP;
	HANDSHAKE;
	return f;
}

TEST_TEAR_DOWN(open)
{
	struct openFixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST_CASE(open, success, NULL)
{
	struct openFixture *f = data;
	(void)params;
	OPEN;
	return MUNIT_OK;
}

/******************************************************************************
 *
 * Handle an prepare request
 *
 ******************************************************************************/

TEST_SUITE(prepare);

struct prepareFixture
{
	FIXTURE;
};

TEST_SETUP(prepare)
{
	struct prepareFixture *f = munit_malloc(sizeof *f);
	SETUP;
	HANDSHAKE;
	OPEN;
	return f;
}

TEST_TEAR_DOWN(prepare)
{
	struct prepareFixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST_CASE(prepare, success, NULL)
{
	struct prepareFixture *f = data;
	unsigned stmtId;
	(void)params;
	PREPARE("CREATE TABLE test (n INT)", &stmtId);
	munit_assert_int(stmtId, ==, 0);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * Handle an exec
 *
 ******************************************************************************/

TEST_SUITE(exec);

struct execFixture
{
	FIXTURE;
	unsigned stmtId;
};

TEST_SETUP(exec)
{
	struct execFixture *f = munit_malloc(sizeof *f);
	SETUP;
	HANDSHAKE;
	OPEN;
	return f;
}

TEST_TEAR_DOWN(exec)
{
	struct execFixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST_CASE(exec, success, NULL)
{
	struct execFixture *f = data;
	unsigned lastInsertId;
	unsigned rowsAffected;
	(void)params;
	PREPARE("CREATE TABLE test (n INT)", &f->stmtId);
	EXEC(f->stmtId, &lastInsertId, &rowsAffected, 8);
	munit_assert_int(lastInsertId, ==, 0);
	munit_assert_int(rowsAffected, ==, 0);
	return MUNIT_OK;
}

TEST_CASE(exec, result, NULL)
{
	struct execFixture *f = data;
	unsigned lastInsertId;
	unsigned rowsAffected;
	(void)params;
	PREPARE("BEGIN", &f->stmtId);
	EXEC(f->stmtId, &lastInsertId, &rowsAffected, 2);
	PREPARE("CREATE TABLE test (n INT)", &f->stmtId);
	EXEC(f->stmtId, &lastInsertId, &rowsAffected, 5);
	PREPARE("INSERT INTO test (n) VALUES(123)", &f->stmtId);
	EXEC(f->stmtId, &lastInsertId, &rowsAffected, 2);
	PREPARE("COMMIT", &f->stmtId);
	EXEC(f->stmtId, &lastInsertId, &rowsAffected, 5);
	munit_assert_int(lastInsertId, ==, 1);
	munit_assert_int(rowsAffected, ==, 1);
	return MUNIT_OK;
}

TEST_CASE(exec, closeWhileInFlight, NULL)
{
	struct execFixture *f = data;
	unsigned lastInsertId;
	unsigned rowsAffected;
	int rv;
	(void)params;

	EXEC_SQL("CREATE TABLE test (n)", &lastInsertId, &rowsAffected, 7);
	rv = clientSendExecSQL(&f->client, "INSERT INTO test(n) VALUES(1)");
	munit_assert_int(rv, ==, 0);

	testUvRun(&f->loop, 1);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * Handle a query
 *
 ******************************************************************************/

TEST_SUITE(query);

struct queryFixture
{
	FIXTURE;
	unsigned stmtId;
	unsigned insertStmtId;
	unsigned lastInsertId;
	unsigned rowsAffected;
	struct rows rows;
};

TEST_SETUP(query)
{
	struct queryFixture *f = munit_malloc(sizeof *f);
	unsigned stmtId;
	SETUP;
	HANDSHAKE;
	OPEN;
	PREPARE("CREATE TABLE test (n INT)", &stmtId);
	EXEC(stmtId, &f->lastInsertId, &f->rowsAffected, 7);
	PREPARE("INSERT INTO test(n) VALUES (123)", &f->insertStmtId);
	EXEC(f->insertStmtId, &f->lastInsertId, &f->rowsAffected, 4);
	return f;
}

TEST_TEAR_DOWN(query)
{
	struct queryFixture *f = data;
	clientCloseRows(&f->rows);
	TEAR_DOWN;
	free(f);
}

/* Perform a query yielding one row. */
TEST_CASE(query, one, NULL)
{
	struct queryFixture *f = data;
	struct row *row;
	(void)params;
	PREPARE("SELECT n FROM test", &f->stmtId);
	QUERY(f->stmtId, &f->rows);
	munit_assert_int(f->rows.columnCount, ==, 1);
	munit_assert_string_equal(f->rows.columnNames[0], "n");
	row = f->rows.next;
	munit_assert_ptr_not_null(row);
	munit_assert_ptr_null(row->next);
	munit_assert_int(row->values[0].type, ==, SQLITE_INTEGER);
	munit_assert_int(row->values[0].integer, ==, 123);
	return MUNIT_OK;
}
