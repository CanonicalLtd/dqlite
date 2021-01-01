#include <stdio.h>
#include <raft.h>

#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/sqlite.h"

#include "../../include/dqlite.h"

SUITE(vfs);

#define N_VFS 2

struct fixture
{
	struct sqlite3_vfs vfs[N_VFS]; /* A "cluster" of VFS objects. */
	char names[8][N_VFS];          /* Registration names */
};

static void *setUp(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	unsigned i;
	int rv;

	SETUP_HEAP;
	SETUP_SQLITE;

	for (i = 0; i < N_VFS; i++) {
		sprintf(f->names[i], "%u", i + 1);
		rv = dqlite_vfs_init(&f->vfs[i], f->names[i]);
		munit_assert_int(rv, ==, 0);
		rv = sqlite3_vfs_register(&f->vfs[i], 0);
		munit_assert_int(rv, ==, 0);
	}

	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	unsigned i;
	int rv;

	for (i = 0; i < N_VFS; i++) {
		rv = sqlite3_vfs_unregister(&f->vfs[i]);
		munit_assert_int(rv, ==, 0);
		dqlite_vfs_close(&f->vfs[i]);
	}

	TEAR_DOWN_SQLITE;
	TEAR_DOWN_HEAP;

	free(f);
}

#define PAGE_SIZE 512

#define PRAGMA(DB, COMMAND)                                          \
	_rv = sqlite3_exec(DB, "PRAGMA " COMMAND, NULL, NULL, NULL); \
	if (_rv != SQLITE_OK) {                                      \
		munit_errorf("PRAGMA " COMMAND ": %s (%d)",          \
			     sqlite3_errmsg(DB), _rv);               \
	}

/* Open a new database connection on the given VFS. */
#define OPEN(VFS, DB)                                                         \
	do {                                                                  \
		int _flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;      \
		int _rv;                                                      \
		_rv = sqlite3_open_v2("test.db", &DB, _flags, VFS);           \
		munit_assert_int(_rv, ==, SQLITE_OK);                         \
		_rv = sqlite3_extended_result_codes(DB, 1);                   \
		munit_assert_int(_rv, ==, SQLITE_OK);                         \
		PRAGMA(DB, "page_size=512");                                  \
		PRAGMA(DB, "synchronous=OFF");                                \
		PRAGMA(DB, "journal_mode=WAL");                               \
		PRAGMA(DB, "cache_size=1");                                   \
		_rv = sqlite3_db_config(DB, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, \
					1, NULL);                             \
		munit_assert_int(_rv, ==, SQLITE_OK);                         \
	} while (0)

/* Close a database connection. */
#define CLOSE(DB)                                     \
	do {                                          \
		int _rv;                              \
		_rv = sqlite3_close(DB);              \
		munit_assert_int(_rv, ==, SQLITE_OK); \
	} while (0)

/* Prepare a statement. */
#define PREPARE(DB, STMT, SQL)                                      \
	do {                                                        \
		int _rv;                                            \
		_rv = sqlite3_prepare_v2(DB, SQL, -1, &STMT, NULL); \
		if (_rv != SQLITE_OK) {                             \
			munit_errorf("prepare '%s': %s (%d)", SQL,  \
				     sqlite3_errmsg(DB), _rv);      \
		}                                                   \
	} while (0)

/* Reset a statement. */
#define RESET(STMT, RV)                        \
	do {                                   \
		int _rv;                       \
		_rv = sqlite3_reset(STMT);     \
		munit_assert_int(_rv, ==, RV); \
	} while (0)

/* Finalize a statement. */
#define FINALIZE(STMT)                                \
	do {                                          \
		int _rv;                              \
		_rv = sqlite3_finalize(STMT);         \
		munit_assert_int(_rv, ==, SQLITE_OK); \
	} while (0)

/* Shortcut for PREPARE, STEP, FINALIZE. */
#define EXEC(DB, SQL)                     \
	do {                              \
		sqlite3_stmt *_stmt;      \
		PREPARE(DB, _stmt, SQL);  \
		STEP(_stmt, SQLITE_DONE); \
		FINALIZE(_stmt);          \
	} while (0)

/* Step through a statement and assert that the given value is returned. */
#define STEP(STMT, RV)                                                        \
	do {                                                                  \
		int _rv;                                                      \
		_rv = sqlite3_step(STMT);                                     \
		if (_rv != RV) {                                              \
			munit_errorf("step: %s (%d)",                         \
				     sqlite3_errmsg(sqlite3_db_handle(STMT)), \
				     _rv);                                    \
		}                                                             \
	} while (0)

/* Hold WAL replication information about a single transaction. */
struct tx
{
	unsigned n;
	unsigned long *pageNumbers;
	void *frames;
};

/* Poll the given VFS object and serialize the transaction data into the given
 * tx object. */
#define POLL(VFS, TX)                                                     \
	do {                                                              \
		sqlite3_vfs *vfs = sqlite3_vfs_find(VFS);                 \
		dqlite_vfs_frame *_frames;                                \
		unsigned _i;                                              \
		int _rv;                                                  \
		memset(&TX, 0, sizeof TX);                                \
		_rv = dqlite_vfs_poll(vfs, "test.db", &_frames, &TX.n);   \
		munit_assert_int(_rv, ==, 0);                             \
		if (_frames != NULL) {                                    \
			TX.pageNumbers =                                  \
			    munit_malloc(sizeof *TX.pageNumbers * TX.n);  \
			TX.frames = munit_malloc(PAGE_SIZE * TX.n);       \
			for (_i = 0; _i < TX.n; _i++) {                   \
				dqlite_vfs_frame *_frame = &_frames[_i];  \
				TX.pageNumbers[_i] = _frame->page_number; \
				memcpy(TX.frames + _i * PAGE_SIZE,        \
				       _frame->data, PAGE_SIZE);          \
				sqlite3_free(_frame->data);               \
			}                                                 \
			sqlite3_free(_frames);                            \
		}                                                         \
	} while (0)

/* Apply WAL frames to the given VFS. */
#define APPLY(VFS, TX)                                                       \
	do {                                                                 \
		sqlite3_vfs *vfs = sqlite3_vfs_find(VFS);                    \
		int _rv;                                                     \
		_rv = dqlite_vfs_apply(vfs, "test.db", TX.n, TX.pageNumbers, \
				       TX.frames);                           \
		munit_assert_int(_rv, ==, 0);                                \
	} while (0)

/* Abort a transaction on the given VFS. */
#define ABORT(VFS)                                        \
	do {                                              \
		sqlite3_vfs *vfs = sqlite3_vfs_find(VFS); \
		int _rv;                                  \
		_rv = dqlite_vfs_abort(vfs, "test.db");   \
		munit_assert_int(_rv, ==, 0);             \
	} while (0)

/* Release all memory used by a struct tx object. */
#define DONE(TX)                      \
	do {                          \
		free(TX.frames);      \
		free(TX.pageNumbers); \
	} while (0)

/* Peform a full checkpoint on the given database. */
#define CHECKPOINT(DB)                                                       \
	do {                                                                 \
		int _size;                                                   \
		int _ckpt;                                                   \
		int _rv;                                                     \
		_rv = sqlite3_wal_checkpoint_v2(                             \
		    DB, "main", SQLITE_CHECKPOINT_TRUNCATE, &_size, &_ckpt); \
		if (_rv != SQLITE_OK) {                                      \
			munit_errorf("checkpoint: %s (%d)",                  \
				     sqlite3_errmsg(DB), _rv);               \
		}                                                            \
	} while (0)

struct snapshot
{
	void *data;
	size_t n;
};

/* Take a snapshot of the database on the given VFS. */
#define SNAPSHOT(VFS, SNAPSHOT)                                           \
	do {                                                              \
		sqlite3_vfs *vfs = sqlite3_vfs_find(VFS);                 \
		int _rv;                                                  \
		_rv = dqlite_vfs_snapshot(vfs, "test.db", &SNAPSHOT.data, \
					  &SNAPSHOT.n);                   \
		munit_assert_int(_rv, ==, 0);                             \
	} while (0)

/* Restore a snapshot onto the given VFS. */
#define RESTORE(VFS, SNAPSHOT)                                          \
	do {                                                            \
		sqlite3_vfs *vfs = sqlite3_vfs_find(VFS);               \
		int _rv;                                                \
		_rv = dqlite_vfs_restore(vfs, "test.db", SNAPSHOT.data, \
					 SNAPSHOT.n);                   \
		munit_assert_int(_rv, ==, 0);                           \
	} while (0)

/* Open and close a new connection using the dqlite VFS. */
TEST(vfs, open, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	OPEN("1", db);
	CLOSE(db);
	return MUNIT_OK;
}

/* New frames appended to the WAL file by a sqlite3_step() call that has
 * triggered a write transactions are not immediately visible to other
 * connections after sqlite3_step() has returned. */
TEST(vfs, writeTransactionNotImmediatelyVisible, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	int rv;

	OPEN("1", db1);
	EXEC(db1, "CREATE TABLE test(n INT)");

	OPEN("1", db2);
	rv = sqlite3_prepare_v2(db2, "SELECT * FROM test", -1, &stmt, NULL);
	munit_assert_int(rv, ==, SQLITE_ERROR);
	munit_assert_string_equal(sqlite3_errmsg(db2), "no such table: test");

	CLOSE(db1);
	CLOSE(db2);

	return MUNIT_OK;
}

/* Invoking dqlite_vfs_poll() after a call to sqlite3_step() has triggered a
 * write transaction returns the newly appended WAL frames. */
TEST(vfs, pollAfterWriteTransaction, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;
	unsigned i;

	OPEN("1", db);

	PREPARE(db, stmt, "CREATE TABLE test(n INT)");
	STEP(stmt, SQLITE_DONE);

	POLL("1", tx);

	munit_assert_ptr_not_null(tx.frames);
	munit_assert_int(tx.n, ==, 2);
	for (i = 0; i < tx.n; i++) {
		munit_assert_int(tx.pageNumbers[i], ==, i + 1);
	}

	DONE(tx);

	FINALIZE(stmt);
	CLOSE(db);

	return MUNIT_OK;
}

/* Invoking dqlite_vfs_poll() after a call to sqlite3_step() has triggered a
 * write transaction sets a write lock on the WAL, so calls to sqlite3_step()
 * from other connections return SQLITE_BUSY if they try to start a write
 * transaction. */
TEST(vfs, pollAcquireWriteLock, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt1;
	sqlite3_stmt *stmt2;
	struct tx tx;

	OPEN("1", db1);
	OPEN("1", db2);

	PREPARE(db1, stmt1, "CREATE TABLE test(n INT)");
	PREPARE(db2, stmt2, "CREATE TABLE test2(n INT)");

	STEP(stmt1, SQLITE_DONE);
	POLL("1", tx);

	STEP(stmt2, SQLITE_BUSY);
	RESET(stmt2, SQLITE_BUSY);

	FINALIZE(stmt1);
	FINALIZE(stmt2);

	CLOSE(db1);
	CLOSE(db2);

	DONE(tx);

	return MUNIT_OK;
}

/* If the page cache limit is exceeded during a call to sqlite3_step() that has
 * triggered a write transaction, some WAL frames will be written and then
 * overwritten before the final commit. Only the final version of the frame is
 * included in the set returned by dqlite_vfs_poll(). */
TEST(vfs, pollAfterPageStress, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;
	unsigned i;
	char sql[64];

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "BEGIN");
	for (i = 0; i < 163; i++) {
		sprintf(sql, "INSERT INTO test(n) VALUES(%d)", i + 1);
		EXEC(db, sql);
		POLL("1", tx);
		munit_assert_int(tx.n, ==, 0);
	}
	for (i = 0; i < 163; i++) {
		sprintf(sql, "UPDATE test SET n=%d WHERE n=%d", i, i + 1);
		EXEC(db, sql);
		POLL("1", tx);
		munit_assert_int(tx.n, ==, 0);
	}
	EXEC(db, "COMMIT");

	POLL("1", tx);

	/* Five frames were replicated and the first frame actually contains a
	 * spill of the third page. */
	munit_assert_int(tx.n, ==, 6);
	munit_assert_int(tx.pageNumbers[0], ==, 3);
	munit_assert_int(tx.pageNumbers[1], ==, 4);
	munit_assert_int(tx.pageNumbers[2], ==, 5);
	munit_assert_int(tx.pageNumbers[3], ==, 1);
	munit_assert_int(tx.pageNumbers[4], ==, 2);

	APPLY("1", tx);
	DONE(tx);

	/* All records have been inserted. */
	PREPARE(db, stmt, "SELECT * FROM test");
	for (i = 0; i < 163; i++) {
		STEP(stmt, SQLITE_ROW);
		munit_assert_int(sqlite3_column_int(stmt, 0), ==, i);
	}
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to actually modify the WAL after a write transaction
 * was triggered by a call to sqlite3_step(), then perform a read transaction
 * and check that it can see the transaction changes. */
TEST(vfs, applyMakesTransactionVisible, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to actually modify the WAL after a write transaction
 * was triggered by an explicit "COMMIT" statement and check that changes are
 * visible. */
TEST(vfs, applyExplicitTransaction, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db);

	PREPARE(db, stmt, "BEGIN");
	STEP(stmt, SQLITE_DONE);
	POLL("1", tx);
	munit_assert_int(tx.n, ==, 0);
	FINALIZE(stmt);

	PREPARE(db, stmt, "CREATE TABLE test(n INT)");
	STEP(stmt, SQLITE_DONE);
	POLL("1", tx);
	munit_assert_int(tx.n, ==, 0);
	FINALIZE(stmt);

	PREPARE(db, stmt, "COMMIT");
	STEP(stmt, SQLITE_DONE);
	POLL("1", tx);
	munit_assert_int(tx.n, ==, 2);
	APPLY("1", tx);
	DONE(tx);
	FINALIZE(stmt);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Perform two consecutive full write transactions using sqlite3_step(),
 * dqlite_vfs_poll() and dqlite_vfs_apply(), then run a read transaction and
 * check that it can see all committed changes. */
TEST(vfs, consecutiveWriteTransactions, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "INSERT INTO test(n) VALUES(123)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_ROW);
	munit_assert_int(sqlite3_column_int(stmt, 0), ==, 123);
	STEP(stmt, SQLITE_DONE);

	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Perform three consecutive write transactions, then re-open the database and
 * finally run a read transaction and check that it can see all committed
 * changes. */
TEST(vfs, reopenAfterConsecutiveWriteTransactions, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE foo(id INT)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "CREATE TABLE bar (id INT)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "INSERT INTO foo(id) VALUES(1)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CLOSE(db);

	OPEN("1", db);

	PREPARE(db, stmt, "SELECT * FROM sqlite_master");
	STEP(stmt, SQLITE_ROW);
	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to actually modify the WAL after a write transaction
 * was triggered by sqlite3_step(), and verify that the transaction is visible
 * from another existing connection. */
TEST(vfs, transactionIsVisibleFromExistingConnection, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);
	OPEN("1", db2);

	EXEC(db1, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	PREPARE(db2, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db1);
	CLOSE(db2);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to actually modify the WAL after a write transaction
 * was triggered by sqlite3_step(), and verify that the transaction is visible
 * from a brand new connection. */
TEST(vfs, transactionIsVisibleFromNewConnection, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);

	EXEC(db1, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	OPEN("1", db2);

	PREPARE(db2, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db1);
	CLOSE(db2);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to actually modify the WAL after a write transaction
 * was triggered by sqlite3_step(), then close the connection and open a new
 * one. A read transaction started in the new connection can see the changes
 * committed by the first one. */
TEST(vfs, transactionIsVisibleFromReopenedConnection, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CLOSE(db);

	OPEN("1", db);
	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);
	CLOSE(db);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to replicate the very first write transaction on a
 * different VFS than the one that initially generated it. In that case it's
 * necessary to initialize the database file on the other VFS by opening and
 * closing a connection. */
TEST(vfs, firstApplyOnDifferentVfs, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);

	PREPARE(db1, stmt, "CREATE TABLE test(n INT)");
	STEP(stmt, SQLITE_DONE);

	POLL("1", tx);

	APPLY("1", tx);

	OPEN("2", db2);
	CLOSE(db2);
	APPLY("2", tx);

	DONE(tx);

	FINALIZE(stmt);
	CLOSE(db1);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to replicate a second write transaction on a different
 * VFS than the one that initially generated it. In that case it's not necessary
 * to do anything special before calling dqlite_vfs_apply(). */
TEST(vfs, secondApplyOnDifferentVfs, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	struct tx tx;

	OPEN("1", db1);

	EXEC(db1, "CREATE TABLE test(n INT)");

	POLL("1", tx);

	APPLY("1", tx);

	OPEN("2", db2);
	CLOSE(db2);
	APPLY("2", tx);

	DONE(tx);

	EXEC(db1, "INSERT INTO test(n) VALUES(123)");

	POLL("1", tx);
	APPLY("1", tx);
	APPLY("2", tx);
	DONE(tx);

	CLOSE(db1);

	return MUNIT_OK;
}

/* Use dqlite_vfs_apply() to replicate a second write transaction on a different
 * VFS than the one that initially generated it and that has an open connection
 * which has built the WAL index header by preparing a statement. */
TEST(vfs, applyOnDifferentVfsWithOpenConnection, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);

	PREPARE(db1, stmt, "CREATE TABLE test(n INT)");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	POLL("1", tx);
	APPLY("1", tx);
	OPEN("2", db2);
	CLOSE(db2);
	APPLY("2", tx);
	DONE(tx);

	EXEC(db1, "INSERT INTO test(n) VALUES(123)");

	POLL("1", tx);

	CLOSE(db1);

	OPEN("2", db2);
	PREPARE(db2, stmt, "PRAGMA cache_size=-5000");
	FINALIZE(stmt);

	APPLY("2", tx);

	PREPARE(db2, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_ROW);
	FINALIZE(stmt);

	DONE(tx);

	CLOSE(db2);

	return MUNIT_OK;
}

/* A write transaction that gets replicated to a different VFS is visible to a
 * new connection opened on that VFS. */
TEST(vfs, transactionVisibleOnDifferentVfs, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);

	EXEC(db1, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	OPEN("2", db2);
	CLOSE(db2);
	APPLY("2", tx);
	DONE(tx);

	CLOSE(db1);

	OPEN("2", db1);
	PREPARE(db1, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);
	CLOSE(db1);

	return MUNIT_OK;
}

/* Calling dqlite_vfs_abort() to cancel a transaction releases the write
 * lock on the WAL. */
TEST(vfs, abort, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt1;
	sqlite3_stmt *stmt2;
	struct tx tx;

	OPEN("1", db1);
	OPEN("1", db2);

	PREPARE(db1, stmt1, "CREATE TABLE test(n INT)");
	PREPARE(db2, stmt2, "CREATE TABLE test2(n INT)");

	STEP(stmt1, SQLITE_DONE);
	POLL("1", tx);
	ABORT("1");

	STEP(stmt2, SQLITE_DONE);

	FINALIZE(stmt1);
	FINALIZE(stmt2);

	CLOSE(db1);
	CLOSE(db2);

	DONE(tx);

	return MUNIT_OK;
}

/* Perform a checkpoint after a write transaction has completed, then perform
 * another write transaction and check that changes both before and after the
 * checkpoint are visible. */
TEST(vfs, checkpoint, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_stmt *stmt;
	struct tx tx;

	OPEN("1", db1);

	EXEC(db1, "CREATE TABLE test(n INT)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);
	EXEC(db1, "INSERT INTO test(n) VALUES(123)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	OPEN("1", db2);
	CHECKPOINT(db2);
	CLOSE(db2);

	EXEC(db1, "INSERT INTO test(n) VALUES(456)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	PREPARE(db1, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_ROW);
	munit_assert_int(sqlite3_column_int(stmt, 0), ==, 123);
	STEP(stmt, SQLITE_ROW);
	munit_assert_int(sqlite3_column_int(stmt, 0), ==, 456);
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db1);

	return MUNIT_OK;
}

/* Replicate a write transaction that happens after a checkpoint. */
TEST(vfs, applyOnDifferentVfsAfterCheckpoint, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx1;
	struct tx tx2;
	struct tx tx3;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");
	POLL("1", tx1);
	APPLY("1", tx1);
	EXEC(db, "INSERT INTO test(n) VALUES(123)");
	POLL("1", tx2);
	APPLY("1", tx2);

	CHECKPOINT(db);

	EXEC(db, "INSERT INTO test(n) VALUES(456)");
	POLL("1", tx3);
	APPLY("1", tx3);

	CLOSE(db);

	OPEN("2", db);
	CLOSE(db);

	APPLY("2", tx1);
	APPLY("2", tx2);

	OPEN("2", db);
	CHECKPOINT(db);
	CLOSE(db);

	APPLY("2", tx3);

	OPEN("2", db);
	PREPARE(db, stmt, "SELECT * FROM test ORDER BY n");
	STEP(stmt, SQLITE_ROW);
	munit_assert_int(sqlite3_column_int(stmt, 0), ==, 123);
	STEP(stmt, SQLITE_ROW);
	munit_assert_int(sqlite3_column_int(stmt, 0), ==, 456);
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);
	CLOSE(db);

	DONE(tx1);
	DONE(tx2);
	DONE(tx3);

	return MUNIT_OK;
}

/* Replicate to another VFS a series of changes including a checkpoint, then
 * perform a new write transaction on that other VFS. */
TEST(vfs, checkpointThenPerformTransaction, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	struct tx tx1;
	struct tx tx2;
	struct tx tx3;

	OPEN("1", db1);

	EXEC(db1, "CREATE TABLE test(n INT)");
	POLL("1", tx1);
	APPLY("1", tx1);
	EXEC(db1, "INSERT INTO test(n) VALUES(123)");
	POLL("1", tx2);
	APPLY("1", tx2);

	CHECKPOINT(db1);

	EXEC(db1, "INSERT INTO test(n) VALUES(456)");
	POLL("1", tx3);
	APPLY("1", tx3);

	CLOSE(db1);

	OPEN("2", db1);

	APPLY("2", tx1);
	APPLY("2", tx2);

	OPEN("2", db2);
	CHECKPOINT(db2);
	CLOSE(db2);

	APPLY("2", tx3);

	DONE(tx1);
	DONE(tx2);
	DONE(tx3);

	EXEC(db1, "INSERT INTO test(n) VALUES(789)");
	POLL("2", tx1);
	APPLY("2", tx1);
	DONE(tx1);

	CLOSE(db1);

	return MUNIT_OK;
}


/* Rollback a transaction that didn't hit the page cache limit and hence didn't
 * perform any pre-commit WAL writes. */
TEST(vfs, rollbackTransactionWithoutPageStress, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	struct tx tx;
	sqlite3_stmt *stmt;

	OPEN("1", db);
	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "BEGIN");
	EXEC(db, "INSERT INTO test(n) VALUES(1)");
	EXEC(db, "ROLLBACK");

	POLL("1", tx);
	munit_assert_int(tx.n, ==, 0);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	RESET(stmt, SQLITE_OK);

	EXEC(db, "INSERT INTO test(n) VALUES(1)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	STEP(stmt, SQLITE_ROW);

	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* Rollback a transaction that hit the page cache limit and hence performed some
 * pre-commit WAL writes. */
TEST(vfs, rollbackTransactionWithPageStress, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct tx tx;
	unsigned i;

	OPEN("1", db);

	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	EXEC(db, "BEGIN");
	for (i = 0; i < 163; i++) {
		char sql[64];
		sprintf(sql, "INSERT INTO test(n) VALUES(%d)", i + 1);
		EXEC(db, sql);
		POLL("1", tx);
		munit_assert_int(tx.n, ==, 0);
	}
	EXEC(db, "ROLLBACK");

	POLL("1", tx);
	munit_assert_int(tx.n, ==, 0);
	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	RESET(stmt, SQLITE_OK);

	EXEC(db, "INSERT INTO test(n) VALUES(1)");
	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	STEP(stmt, SQLITE_ROW);

	FINALIZE(stmt);

	CLOSE(db);

	return MUNIT_OK;
}

/* A snapshot of a brand new database that has been just initialized contains
 * just the first page of the main database file. */
TEST(vfs, snapshotInitialDatabase, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	struct snapshot snapshot;
	uint8_t *page;
	uint8_t pageSize[2] = {2, 0};            /* Big-endian page size */
	uint8_t databaseSize[4] = {0, 0, 0, 1};  /* Big-endian database size */

	OPEN("1", db);
	CLOSE(db);

	SNAPSHOT("1", snapshot);

	munit_assert_int(snapshot.n, ==, PAGE_SIZE);
	page = snapshot.data;

	munit_assert_int(memcmp(&page[16], pageSize, 2), ==, 0);
	munit_assert_int(memcmp(&page[28], databaseSize, 4), ==, 0);

	raft_free(snapshot.data);

	return MUNIT_OK;
}

/* A snapshot of a database after the first write transaction gets applied
 * contains the first page of the database plus the WAL file containing the
 * transaction frames. */
TEST(vfs, snapshotAfterFirstTransaction, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	struct snapshot snapshot;
	struct tx tx;
	uint8_t *page;
	uint8_t pageSize[2] = {2, 0};            /* Big-endian page size */
	uint8_t databaseSize[4] = {0, 0, 0, 1};  /* Big-endian database size */

	OPEN("1", db);
	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CLOSE(db);

	SNAPSHOT("1", snapshot);

	munit_assert_int(snapshot.n, ==, PAGE_SIZE + 32 + (24 + PAGE_SIZE) * 2);
	page = snapshot.data;

	munit_assert_int(memcmp(&page[16], pageSize, 2), ==, 0);
	munit_assert_int(memcmp(&page[28], databaseSize, 4), ==, 0);

	raft_free(snapshot.data);

	return MUNIT_OK;
}

/* A snapshot of a database after a checkpoint contains all checkpointed pages
 * and no WAL frames. */
TEST(vfs, snapshotAfterCheckpoint, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	struct snapshot snapshot;
	struct tx tx;
	uint8_t *page;
	uint8_t pageSize[2] = {2, 0};            /* Big-endian page size */
	uint8_t databaseSize[4] = {0, 0, 0, 2};  /* Big-endian database size */

	OPEN("1", db);
	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CHECKPOINT(db);

	CLOSE(db);

	SNAPSHOT("1", snapshot);

	munit_assert_int(snapshot.n, ==, PAGE_SIZE * 2);
	page = snapshot.data;

	munit_assert_int(memcmp(&page[16], pageSize, 2), ==, 0);
	munit_assert_int(memcmp(&page[28], databaseSize, 4), ==, 0);

	raft_free(snapshot.data);

	return MUNIT_OK;
}

/* Restore a snapshot taken after a brand new database has been just
 * initialized. */
TEST(vfs, restoreInitialDatabase, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	struct snapshot snapshot;

	OPEN("1", db);
	CLOSE(db);

	SNAPSHOT("1", snapshot);

	OPEN("2", db);
	CLOSE(db);

	RESTORE("2", snapshot);

	raft_free(snapshot.data);

	return MUNIT_OK;
}

/* Restore a snapshot of a database taken after the first write transaction gets
 * applied. */
TEST(vfs, restoreAfterFirstTransaction, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct snapshot snapshot;
	struct tx tx;

	OPEN("1", db);
	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CLOSE(db);

	SNAPSHOT("1", snapshot);

	OPEN("2", db);
	CLOSE(db);

	RESTORE("2", snapshot);

	OPEN("2", db);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db);

	raft_free(snapshot.data);

	return MUNIT_OK;
}

/* Restore a snapshot of a database while a connection is open. */
TEST(vfs, restoreWithOpenConnection, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	struct snapshot snapshot;
	struct tx tx;

	OPEN("1", db);
	EXEC(db, "CREATE TABLE test(n INT)");

	POLL("1", tx);
	APPLY("1", tx);
	DONE(tx);

	CLOSE(db);

	SNAPSHOT("1", snapshot);

	OPEN("2", db);

	RESTORE("2", snapshot);

	PREPARE(db, stmt, "SELECT * FROM test");
	STEP(stmt, SQLITE_DONE);
	FINALIZE(stmt);

	CLOSE(db);

	raft_free(snapshot.data);

	return MUNIT_OK;
}
