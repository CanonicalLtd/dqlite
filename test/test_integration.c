#include <pthread.h>
#include <time.h>

#include "../include/dqlite.h"

#include "./lib/runner.h"

TEST_MODULE(integration);

#if 0

/******************************************************************************
 *
 * Helpers
 *
 ******************************************************************************/

/* A worker that keeps inserting rows into a test table and fetching them back,
 * checking that they have been all inserted. */
struct worker
{
	struct testClient *client; /* A connected client */
	int i;			    /* Worker index */
	int a;			    /* Start inserting from this number */
	int n;			    /* Number of insertions to perform */
	pthread_t thread;	   /* System thread we run in */
};

static void *__worker_run(void *arg)
{
	struct worker *w;
	char *leader;
	uint64_t heartbeat;
	uint32_t dbId;
	int b;
	int i;

	munit_assert_ptr_not_null(arg);

	w = (struct worker *)arg;

	/* Initialize the connection and open a database. */
	testClientHandshake(w->client);
	testClientLeader(w->client, &leader);
	testClientClient(w->client, &heartbeat);
	testClientOpen(w->client, "test.db", &dbId);

	b = w->a + w->n;

	for (i = w->a; i < b; i++) {
		uint32_t stmtId;
		char sql[128];
		struct testClientResult result;
		struct testClientRows rows;
		struct testClientRow *row;
		int j;

		/* Insert a row in the test table. */
		sprintf(sql, "INSERT INTO test(n) VALUES(%d)", i);

		testClientPrepare(w->client, dbId, sql, &stmtId);
		testClientExec(w->client, dbId, stmtId, &result);

		munit_assert_int(result.rowsAffected, ==, 1);

		testClientFinalize(w->client, dbId, stmtId);

		/* Fetch all rows within our own working range. */
		sprintf(sql, "SELECT n FROM test WHERE n >= %d AND n < %d",
			w->a, b);

		testClientPrepare(w->client, dbId, sql, &stmtId);
		testClientQuery(w->client, dbId, stmtId, &rows);

		munit_assert_int(rows.columnCount, ==, 1);
		munit_assert_string_equal(rows.columnNames[0], "n");

		row = rows.next;
		for (j = w->a; j <= i; j++) {
			munit_assert_ptr_not_null(row);

			munit_assert_int(row->types[0], ==, SQLITE_INTEGER);
			munit_assert_int(*(int64_t *)row->values[0], ==, j);

			row = row->next;
		}

		testClientRowsClose(&rows);
		testClientFinalize(w->client, dbId, stmtId);
	}

	return 0;
}

static void __worker_start(struct worker *w,
			   struct testServer *server,
			   int i,
			   int a,
			   int n)
{
	int err;

	w->i = i;
	w->a = a;
	w->n = n;

	testServerConnect(server, &w->client);

	err = pthread_create(&w->thread, 0, &__worker_run, (void *)w);
	if (err) {
		munit_errorf("failed to spawn test worker thread: %s",
			     strerror(errno));
	}
}

static void __worker_wait(struct worker *w)
{
	int err;
	void *retval;

	err = pthread_join(w->thread, &retval);
	if (err) {
		munit_errorf("failed to wait test worker thread: %s",
			     strerror(errno));
	}

	testClientClose(w->client);
	free(w->client);
}

/******************************************************************************
 *
 * Setup and tear down
 *
 ******************************************************************************/

static void *setup(const MunitParameter params[], void *userData)
{
	struct testServer *server;
	const char *errmsg;
	int err;

	(void)userData;
	(void)params;

	err = dqliteInit(&errmsg);
	munit_assert_int(err, ==, 0);

	server = testServerStart("unix", params);

	return server;
}

static void tear_down(void *data)
{
	struct testServer *server = data;
	int rc;

	testServerStop(server);

	rc = sqlite3_shutdown();
	munit_assert_int(rc, ==, 0);
}

/******************************************************************************
 *
 * Tests
 *
 ******************************************************************************/

TEST_SUITE(exec);
TEST_SETUP(exec, setup);
TEST_TEAR_DOWN(exec, tear_down);

#include <unistd.h>

TEST_CASE(exec, singleQuery, NULL)
{
	struct testServer *server = data;
	struct testClient *client;
	char *leader;
	uint64_t heartbeat;
	uint32_t dbId;
	uint32_t stmtId;
	struct testClientResult result;
	struct testClientRows rows;

	(void)params;

	testServerConnect(server, &client);

	/* Initialize the connection and open a database. */
	testClientHandshake(client);
	testClientLeader(client, &leader);
	testClientClient(client, &heartbeat);
	testClientOpen(client, "test.db", &dbId);
	munit_assert_int(dbId, ==, 0);

	/* Create a test table. */
	testClientPrepare(client, dbId, "CREATE TABLE test (n INT)",
			    &stmtId);
	testClientExec(client, dbId, stmtId, &result);
	testClientFinalize(client, dbId, stmtId);

	/* Insert a row in the test table. */
	testClientPrepare(client, dbId, "INSERT INTO test VALUES(123)",
			    &stmtId);

	munit_assert_int(stmtId, ==, 0);

	testClientExec(client, dbId, stmtId, &result);

	munit_assert_int(result.lastInsertId, ==, 1);
	munit_assert_int(result.rowsAffected, ==, 1);

	testClientFinalize(client, dbId, stmtId);

	/* Select rows from the test table. */
	testClientPrepare(client, dbId, "SELECT n FROM test", &stmtId);

	munit_assert_int(stmtId, ==, 0);

	testClientQuery(client, dbId, stmtId, &rows);

	munit_assert_int(rows.columnCount, ==, 1);
	munit_assert_string_equal(rows.columnNames[0], "n");

	munit_assert_ptr_not_null(rows.next);
	munit_assert_int(rows.next->types[0], ==, SQLITE_INTEGER);
	munit_assert_int(*(int64_t *)rows.next->values[0], ==, 123);

	testClientRowsClose(&rows);

	testClientFinalize(client, dbId, stmtId);

	testClientClose(client);
	free(client);

	return MUNIT_OK;
}

TEST_CASE(exec, largeQuery, NULL)
{
	struct testServer *server = data;
	struct testClient *client;
	char *leader;
	uint64_t heartbeat;
	uint32_t dbId;
	uint32_t stmtId;
	struct testClientResult result;
	struct testClientRows rows;
	int i;

	(void)params;

	testServerConnect(server, &client);

	/* Initialize the connection and open a database. */
	testClientHandshake(client);
	testClientLeader(client, &leader);
	testClientClient(client, &heartbeat);
	testClientOpen(client, "test.db", &dbId);
	munit_assert_int(dbId, ==, 0);

	/* Create a test table. */
	testClientPrepare(client, dbId, "CREATE TABLE test (n INT)",
			    &stmtId);
	testClientExec(client, dbId, stmtId, &result);
	testClientFinalize(client, dbId, stmtId);

	testClientPrepare(client, dbId, "BEGIN", &stmtId);
	testClientExec(client, dbId, stmtId, &result);
	testClientFinalize(client, dbId, stmtId);

	/* Insert lots of rows in the test table. */
	testClientPrepare(client, dbId, "INSERT INTO test VALUES(123456789)",
			    &stmtId);

	for (i = 0; i < 256; i++) {
		munit_assert_int(stmtId, ==, 0);
		testClientExec(client, dbId, stmtId, &result);
		munit_assert_int(result.rowsAffected, ==, 1);
	}

	testClientFinalize(client, dbId, stmtId);

	testClientPrepare(client, dbId, "COMMIT", &stmtId);
	testClientExec(client, dbId, stmtId, &result);
	testClientFinalize(client, dbId, stmtId);

	/* Select all rows from the test table. */
	testClientPrepare(client, dbId, "SELECT n FROM test", &stmtId);

	munit_assert_int(stmtId, ==, 0);

	testClientQuery(client, dbId, stmtId, &rows);

	munit_assert_int(rows.columnCount, ==, 1);
	munit_assert_string_equal(rows.columnNames[0], "n");

	munit_assert_ptr_not_null(rows.next);
	munit_assert_int(rows.next->types[0], ==, SQLITE_INTEGER);
	munit_assert_int(*(int64_t *)rows.next->values[0], ==, 123456789);

	testClientRowsClose(&rows);

	testClientFinalize(client, dbId, stmtId);

	testClientClose(client);

	free(client);

	return MUNIT_OK;
}

TEST_CASE(exec, multiThread, NULL)
{
	struct testServer *server = data;
	struct worker *workers;
	struct testClient *client;
	struct testClientResult result;
	char *leader;
	uint64_t heartbeat;
	uint32_t dbId;
	uint32_t stmtId;

	(void)params;

	int n = 2;
	int i;

	testServerConnect(server, &client);

	/* Initialize the connection and open a database. */
	testClientHandshake(client);
	testClientLeader(client, &leader);
	testClientClient(client, &heartbeat);
	testClientOpen(client, "test.db", &dbId);
	munit_assert_int(dbId, ==, 0);

	/* Create a test table and close this client. */
	testClientPrepare(client, dbId, "CREATE TABLE test (n INT)",
			    &stmtId);
	testClientExec(client, dbId, stmtId, &result);
	testClientFinalize(client, dbId, stmtId);

	testClientClose(client);

	/* Spawn the workers. */
	workers = munit_malloc(n * sizeof *workers);

	for (i = 0; i < n; i++) {
		__worker_start(&(workers[i]), server, i, i * 100000, 4);
	}

	/* Wait for the workers. */
	for (i = 0; i < n; i++) {
		__worker_wait(&(workers[i]));
	}

	free(client);
	free(workers);

	return MUNIT_OK;
}

#endif
