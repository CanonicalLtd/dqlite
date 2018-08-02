#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "../include/dqlite.h"

#include "munit.h"

static const char *test__cluster_leader(void *ctx) {
	(void)ctx;
	char *address;

	/* Allocate a string, as regular implementations of the cluster
	 * interface are expected to do. */
	address = malloc(strlen("127.0.0.1:666") + 1);
	if (address == NULL) {
		return NULL;
	}

	strcpy(address, "127.0.0.1:666");

	return address;
}

static int test__cluster_servers_rc = SQLITE_OK;

static int test__cluster_servers(void *ctx, dqlite_server_info **servers) {
	(void)ctx;

	if (test__cluster_servers_rc != 0) {
		*servers = NULL;
		return test__cluster_servers_rc;
	}

	/* Allocate the servers array, as regular implementations of the cluster
	 * interface are expected to do. */
	*servers = malloc(3 * sizeof **servers);

	(*servers)[0].id      = 1;
	(*servers)[0].address = malloc(strlen("1.2.3.4:666") + 1);
	strcpy((char *)(*servers)[0].address, "1.2.3.4:666");

	(*servers)[1].id      = 2;
	(*servers)[1].address = malloc(strlen("5.6.7.8:666") + 1);
	strcpy((char *)(*servers)[1].address, "5.6.7.8:666");

	(*servers)[2].id      = 0;
	(*servers)[2].address = NULL;

	return 0;
}

static void test__cluster_register(void *ctx, sqlite3 *db) {
	(void)ctx;
	(void)db;
}

static void test__cluster_unregister(void *ctx, sqlite3 *db) {
	(void)ctx;
	(void)db;
}

static int test__cluster_barrier(void *ctx) {
	(void)ctx;

	return 0;
}

static int test__cluster_checkpoint(void *ctx, sqlite3 *db) {
	int rc;
	int log;
	int ckpt;

	(void)ctx;

	rc = sqlite3_wal_checkpoint_v2(
	    db, "main", SQLITE_CHECKPOINT_TRUNCATE, &log, &ckpt);
	munit_assert_int(rc, ==, 0);

	munit_assert_int(log, ==, 0);
	munit_assert_int(ckpt, ==, 0);

	return 0;
}

static dqlite_cluster test__cluster = {
    NULL,
    test__cluster_leader,
    test__cluster_servers,
    test__cluster_register,
    test__cluster_unregister,
    test__cluster_barrier,
    NULL,
    test__cluster_checkpoint,
};

dqlite_cluster *test_cluster() { return &test__cluster; }

void test_cluster_servers_rc(int rc) { test__cluster_servers_rc = rc; }
