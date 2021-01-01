#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/server.h"
#include "../lib/sqlite.h"

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Send the initial client handshake. */
#define HANDSHAKE                                     \
	{                                             \
		int rv_;                              \
		rv_ = clientSendHandshake(f->client); \
		munit_assert_int(rv_, ==, 0);         \
	}

/* Open a test database. */
#define OPEN                                             \
	{                                                \
		int rv_;                                 \
		rv_ = clientSendOpen(f->client, "test"); \
		munit_assert_int(rv_, ==, 0);            \
		rv_ = clientRecvDb(f->client);           \
		munit_assert_int(rv_, ==, 0);            \
	}

/* Prepare a statement. */
#define PREPARE(SQL, STMT_ID)                             \
	{                                                 \
		int rv_;                                  \
		rv_ = clientSendPrepare(f->client, SQL);  \
		munit_assert_int(rv_, ==, 0);             \
		rv_ = clientRecvStmt(f->client, STMT_ID); \
		munit_assert_int(rv_, ==, 0);             \
	}

/* Execute a statement. */
#define EXEC(STMT_ID, LAST_INSERT_ID, ROWS_AFFECTED)              \
	{                                                         \
		int rv_;                                          \
		rv_ = clientSendExec(f->client, STMT_ID);         \
		munit_assert_int(rv_, ==, 0);                     \
		rv_ = clientRecvResult(f->client, LAST_INSERT_ID, \
				       ROWS_AFFECTED);            \
		munit_assert_int(rv_, ==, 0);                     \
	}

/* Perform a query. */
#define QUERY(STMT_ID, ROWS)                               \
	{                                                  \
		int rv_;                                   \
		rv_ = clientSendQuery(f->client, STMT_ID); \
		munit_assert_int(rv_, ==, 0);              \
		rv_ = clientRecvRows(f->client, ROWS);     \
		munit_assert_int(rv_, ==, 0);              \
	}

/******************************************************************************
 *
 * Handle client requests
 *
 ******************************************************************************/

SUITE(client);

struct fixture
{
	struct testServer server;
	struct client *client;
};

static void *setUp(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	(void)userData;
	testHeapSetup(params, userData);
	testSqliteSetup(params);
	testServerSetup(&f->server, 1, params);
	testServerStart(&f->server);
	f->client = testServerClient(&f->server);
	HANDSHAKE;
	OPEN;
	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	testServerTearDown(&f->server);
	testSqliteTearDown();
	testHeapTearDown(data);

	free(f);
}

TEST(client, exec, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	unsigned stmtId;
	unsigned lastInsertId;
	unsigned rowsAffected;
	(void)params;
	PREPARE("CREATE TABLE test (n INT)", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);
	return MUNIT_OK;
}

TEST(client, query, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	unsigned stmtId;
	unsigned lastInsertId;
	unsigned rowsAffected;
	unsigned i;
	struct rows rows;
	(void)params;
	PREPARE("CREATE TABLE test (n INT)", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);

	PREPARE("BEGIN", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);

	PREPARE("INSERT INTO test (n) VALUES(123)", &stmtId);
	for (i = 0; i < 256; i++) {
		EXEC(stmtId, &lastInsertId, &rowsAffected);
	}

	PREPARE("COMMIT", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);

	PREPARE("SELECT n FROM test", &stmtId);
	QUERY(stmtId, &rows);

	clientCloseRows(&rows);

	return MUNIT_OK;
}
