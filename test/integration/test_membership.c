#include "../../src/client.h"
#include "../../src/server.h"
#include "../lib/endpoint.h"
#include "../lib/fs.h"
#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/server.h"
#include "../lib/sqlite.h"

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

#define N_SERVERS 3
#define FIXTURE                               \
	struct testServer servers[N_SERVERS]; \
	struct client *client

#define SETUP                                                \
	unsigned i_;                                         \
	testHeapSetup(params, userData);                     \
	testSqliteSetup(params);                             \
	for (i_ = 0; i_ < N_SERVERS; i_++) {                 \
		struct testServer *server = &f->servers[i_]; \
		testServerSetup(server, i_ + 1, params);     \
	}                                                    \
	testServerNetwork(f->servers, N_SERVERS);            \
	for (i_ = 0; i_ < N_SERVERS; i_++) {                 \
		struct testServer *server = &f->servers[i_]; \
		testServerStart(server);                     \
	}                                                    \
	SELECT(1)

#define TEAR_DOWN                                    \
	unsigned i_;                                 \
	for (i_ = 0; i_ < N_SERVERS; i_++) {         \
		testServerTearDown(&f->servers[i_]); \
	}                                            \
	testSqliteTearDown();                        \
	testHeapTearDown(data)

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Use the client connected to the server with the given ID. */
#define SELECT(ID) f->client = testServerClient(&f->servers[ID - 1])

/* Send the initial client handshake. */
#define HANDSHAKE                                     \
	{                                             \
		int rv_;                              \
		rv_ = clientSendHandshake(f->client); \
		munit_assert_int(rv_, ==, 0);         \
	}

/* Send an add request. */
#define ADD(ID, ADDRESS)                                     \
	{                                                    \
		int rv_;                                     \
		rv_ = clientSendAdd(f->client, ID, ADDRESS); \
		munit_assert_int(rv_, ==, 0);                \
		rv_ = clientRecvEmpty(f->client);            \
		munit_assert_int(rv_, ==, 0);                \
	}

/* Send an assign role request. */
#define ASSIGN(ID, ROLE)                                     \
	{                                                    \
		int rv_;                                     \
		rv_ = clientSendAssign(f->client, ID, ROLE); \
		munit_assert_int(rv_, ==, 0);                \
		rv_ = clientRecvEmpty(f->client);            \
		munit_assert_int(rv_, ==, 0);                \
	}

/* Send a remove request. */
#define REMOVE(ID)                                     \
	{                                              \
		int rv_;                               \
		rv_ = clientSendRemove(f->client, ID); \
		munit_assert_int(rv_, ==, 0);          \
		rv_ = clientRecvEmpty(f->client);      \
		munit_assert_int(rv_, ==, 0);          \
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

/******************************************************************************
 *
 * join
 *
 ******************************************************************************/

SUITE(membership)

struct fixture
{
	FIXTURE;
};

static void *setUp(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	TEAR_DOWN;
	free(f);
}

TEST(membership, join, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	unsigned id = 2;
	const char *address = "@2";
	unsigned stmtId;
	unsigned lastInsertId;
	unsigned rowsAffected;

	HANDSHAKE;
	ADD(id, address);
	ASSIGN(id, 1 /* voter */);
	OPEN;
	PREPARE("CREATE TABLE test (n INT)", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);
	PREPARE("INSERT INTO test(n) VALUES(1)", &stmtId);
	EXEC(stmtId, &lastInsertId, &rowsAffected);

	/* The table is visible from the new node */
	SELECT(2);
	HANDSHAKE;
	OPEN;
	PREPARE("SELECT * FROM test", &stmtId);

	/* TODO: fix the standalone test for remove */
	SELECT(1);
	REMOVE(id);
	return MUNIT_OK;
}
