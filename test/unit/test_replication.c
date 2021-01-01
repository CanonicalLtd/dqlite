#include "../lib/cluster.h"
#include "../lib/runner.h"

#include "../../src/format.h"
#include "../../src/leader.h"

TEST_MODULE(replicationV1);

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

#define FIXTURE                           \
	FIXTURE_CLUSTER;                  \
	struct leader leaders[N_SERVERS]; \
	sqlite3_stmt *stmt;

#define SETUP                             \
	unsigned i;                       \
	SETUP_CLUSTER(V2)                 \
	for (i = 0; i < N_SERVERS; i++) { \
		SETUP_LEADER(i);          \
	}

#define SETUP_LEADER(I)                                          \
	do {                                                     \
		struct leader *leader = &f->leaders[I];          \
		struct registry *registry = CLUSTER_REGISTRY(I); \
		struct db *db;                                   \
		int rc2;                                         \
		rc2 = registryDbGet(registry, "test.db", &db);   \
		munit_assert_int(rc2, ==, 0);                    \
		rc2 = leaderInit(leader, db, CLUSTER_RAFT(I));   \
		munit_assert_int(rc2, ==, 0);                    \
	} while (0)

#define TEAR_DOWN                         \
	unsigned i;                       \
	for (i = 0; i < N_SERVERS; i++) { \
		TEAR_DOWN_LEADER(i);      \
	}                                 \
	TEAR_DOWN_CLUSTER

#define TEAR_DOWN_LEADER(I)                             \
	do {                                            \
		struct leader *leader = &f->leaders[I]; \
		leaderClose(leader);                    \
	} while (0)

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Return the i'th leader object. */
#define LEADER(I) &f->leaders[I]

/* Return the SQLite connection of the i'th leader object */
#define CONN(I) (LEADER(I))->conn

/* Prepare the fixture's statement using the connection of the I'th leader */
#define PREPARE(I, SQL)                                                     \
	{                                                                   \
		int rc2;                                                    \
		rc2 = sqlite3_prepare_v2(CONN(I), SQL, -1, &f->stmt, NULL); \
		munit_assert_int(rc2, ==, 0);                               \
	}

/* Reset the fixture's statement, expecting the given return code. */
#define RESET(RC)                              \
	{                                      \
		int rc2;                       \
		rc2 = sqlite3_reset(f->stmt);  \
		munit_assert_int(rc2, ==, RC); \
	}

/* Finalize the fixture's statement */
#define FINALIZE                                 \
	{                                        \
		int rc2;                         \
		rc2 = sqlite3_finalize(f->stmt); \
		munit_assert_int(rc2, ==, 0);    \
	}

/* Submit an exec request using the I'th leader. */
#define EXEC(I)                                                               \
	{                                                                     \
		int rc2;                                                      \
		rc2 = leaderExec(LEADER(I), &f->req, f->stmt, fixtureExecCb); \
		munit_assert_int(rc2, ==, 0);                                 \
	}

/* Convenience to prepare, execute and finalize a statement. */
#define EXEC_SQL(I, SQL)                        \
	PREPARE(I, SQL);                        \
	EXEC(I);                                \
	CLUSTER_APPLIED(CLUSTER_LAST_INDEX(I)); \
	FINALIZE

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

/* Assert the number of pages in the WAL file on the I'th node. */
#define ASSERT_WAL_PAGES(I, N)                                           \
	{                                                                \
		struct leader *leader_ = &f->leaders[I];                 \
		sqlite3_file *file_;                                     \
		sqlite_int64 size_;                                      \
		int pages_;                                              \
		int rv_;                                                 \
		rv_ = sqlite3_file_control(leader_->conn, "main",        \
					   SQLITE_FCNTL_JOURNAL_POINTER, \
					   &file_);                      \
		munit_assert_int(rv_, ==, 0);                            \
		rv_ = file_->pMethods->xFileSize(file_, &size_);         \
		munit_assert_int(rv_, ==, 0);                            \
		pages_ = formatWalCalcFramesNumber(                      \
		    leader_->db->config->pageSize, size_);               \
		munit_assert_int(pages_, ==, N);                         \
	}

/******************************************************************************
 *
 * leaderInit
 *
 ******************************************************************************/

struct initFixture
{
	FIXTURE;
};

TEST_SUITE(init);
TEST_SETUP(init)
{
	struct initFixture *f = munit_malloc(sizeof *f);
	SETUP;
	return f;
}
TEST_TEAR_DOWN(init)
{
	struct initFixture *f = data;
	TEAR_DOWN;
	free(f);
}

/* The connection is open and can be used. */
TEST_CASE(init, conn, NULL)
{
	struct initFixture *f = data;
	sqlite3_stmt *stmt;
	int rc;
	(void)params;
	rc = sqlite3_prepare_v2(CONN(0), "SELECT 1", -1, &stmt, NULL);
	munit_assert_int(rc, ==, 0);
	sqlite3_finalize(stmt);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * leaderExec
 *
 ******************************************************************************/

struct execFixture
{
	FIXTURE;
	struct exec req;
	bool invoked;
	int status;
};

static void fixtureExecCb(struct exec *req, int status)
{
	struct execFixture *f = req->data;
	f->invoked = true;
	f->status = status;
}

TEST_SUITE(exec);
TEST_SETUP(exec)
{
	struct execFixture *f = munit_malloc(sizeof *f);
	SETUP;
	f->req.data = f;
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
	(void)params;
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (a  INT)");
	EXEC(0);
	CLUSTER_APPLIED(2);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, SQLITE_DONE);
	FINALIZE;
	return MUNIT_OK;
}

/* A snapshot is taken after applying an entry. */
TEST_CASE(exec, snapshot, NULL)
{
	struct execFixture *f = data;
	(void)params;
	CLUSTER_SNAPSHOT_THRESHOLD(0, 4);
	CLUSTER_ELECT(0);
	PREPARE(0, "CREATE TABLE test (n  INT)");
	EXEC(0);
	CLUSTER_APPLIED(2);
	FINALIZE;
	PREPARE(0, "INSERT INTO test(n) VALUES(1)");
	EXEC(0);
	CLUSTER_APPLIED(3);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, SQLITE_DONE);
	FINALIZE;
	return MUNIT_OK;
}

/* If a transaction is in progress, no snapshot is taken. */
TEST_CASE(exec, snapshotBusy, NULL)
{
	struct execFixture *f = data;
	(void)params;
	unsigned i;
	CLUSTER_SNAPSHOT_THRESHOLD(0, 4);
	CLUSTER_ELECT(0);
	EXEC_SQL(0, "PRAGMA cache_size = 1");
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");
	EXEC_SQL(0, "BEGIN");
	/* Accumulate enough dirty data to fill the page cache */
	for (i = 0; i < 163; i++) {
		EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");
	}
	return MUNIT_OK;
}

/* If the WAL size grows beyond the configured threshold, checkpoint it. */
TEST_CASE(exec, checkpoint, NULL)
{
	struct execFixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);
	(void)params;
	config->checkpointThreshold = 3;
	CLUSTER_ELECT(0);
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");
	EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");
	/* The WAL was truncated. */
	ASSERT_WAL_PAGES(0, 0);
	return MUNIT_OK;
}

/* If a read transaction is in progress, no checkpoint is taken. */
TEST_CASE(exec, checkpointReadLock, NULL)
{
	struct execFixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);
	struct registry *registry = CLUSTER_REGISTRY(0);
	struct db *db;
	struct leader leader2;
	char *errmsg;
	int rv;
	(void)params;
	config->checkpointThreshold = 3;

	CLUSTER_ELECT(0);
	EXEC_SQL(0, "CREATE TABLE test (n  INT)");

	/* Initialize another leader. */
	rv = registryDbGet(registry, "test.db", &db);
	munit_assert_int(rv, ==, 0);
	leaderInit(&leader2, db, CLUSTER_RAFT(0));

	/* Start a read transaction in the other leader. */
	rv = sqlite3_exec(leader2.conn, "BEGIN", NULL, NULL, &errmsg);
	munit_assert_int(rv, ==, 0);

	rv = sqlite3_exec(leader2.conn, "SELECT * FROM test", NULL, NULL,
			  &errmsg);
	munit_assert_int(rv, ==, 0);

	EXEC_SQL(0, "INSERT INTO test(n) VALUES(1)");

	/* The WAL was not truncated. */
	ASSERT_WAL_PAGES(0, 3);

	leaderClose(&leader2);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture
{
	FIXTURE_CLUSTER;
	struct leader leaders[N_SERVERS];
	sqlite3_stmt *stmt;
	struct exec req;
	bool invoked;
	int status;
};

static void *setUp(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	SETUP_CLUSTER(V2);
	SETUP_LEADER(0);
	f->req.data = f;
	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	TEAR_DOWN_LEADER(0);
	TEAR_DOWN_CLUSTER;
	free(f);
}

SUITE(replication)

static void execCb(struct exec *req, int status)
{
	struct fixture *f = req->data;
	f->invoked = true;
	f->status = status;
}

TEST(replication, exec, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int rv;

	CLUSTER_ELECT(0);

	PREPARE(0, "BEGIN");
	rv = leaderExec(LEADER(0), &f->req, f->stmt, execCb);
	munit_assert_int(rv, ==, 0);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, SQLITE_DONE);
	f->invoked = false;
	FINALIZE;

	PREPARE(0, "CREATE TABLE test (a  INT)");
	rv = leaderExec(LEADER(0), &f->req, f->stmt, execCb);
	munit_assert_int(rv, ==, 0);
	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, SQLITE_DONE);
	f->invoked = false;
	FINALIZE;

	PREPARE(0, "COMMIT");
	rv = leaderExec(LEADER(0), &f->req, f->stmt, execCb);
	munit_assert_int(rv, ==, 0);
	munit_assert_false(f->invoked);
	FINALIZE;

	CLUSTER_APPLIED(2);

	munit_assert_true(f->invoked);
	munit_assert_int(f->status, ==, SQLITE_DONE);

	PREPARE(0, "SELECT * FROM test");
	FINALIZE;

	SETUP_LEADER(1);
	PREPARE(1, "SELECT * FROM test");
	FINALIZE;
	TEAR_DOWN_LEADER(1);

	return MUNIT_OK;
}

/* If the WAL size grows beyond the configured threshold, checkpoint it. */
TEST(replication, checkpoint, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	struct config *config = CLUSTER_CONFIG(0);
	int rv;

	config->checkpointThreshold = 3;

	CLUSTER_ELECT(0);

	PREPARE(0, "CREATE TABLE test (n  INT)");
	rv = leaderExec(LEADER(0), &f->req, f->stmt, execCb);
	munit_assert_int(rv, ==, 0);
	FINALIZE;

	CLUSTER_APPLIED(2);

	PREPARE(0, "INSERT INTO test(n) VALUES(1)");
	rv = leaderExec(LEADER(0), &f->req, f->stmt, execCb);
	munit_assert_int(rv, ==, 0);
	FINALIZE;

	CLUSTER_APPLIED(4);

	/* The WAL was truncated. */
	ASSERT_WAL_PAGES(0, 0);

	return MUNIT_OK;
}
