#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/fs.h"
#include "../lib/sqlite.h"

#include "../../include/dqlite.h"

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture
{
	char *dir;         /* Data directory. */
	dqlite_node *node; /* Node instance. */
};

static void *setUp(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	testHeapSetup(params, userData);
	testSqliteSetup(params);

	f->dir = testDirSetup();

	rv = dqlite_node_create(1, "1", f->dir, &f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(f->node, "@123");
	munit_assert_int(rv, ==, 0);

	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;

	dqlite_node_destroy(f->node);

	testDirTearDown(f->dir);
	testSqliteTearDown();
	testHeapTearDown(data);
	free(f);
}

SUITE(node);

/******************************************************************************
 *
 * dqlite_node_start
 *
 ******************************************************************************/

TEST(node, start, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int rv;

	rv = dqlite_node_start(f->node);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_stop(f->node);
	munit_assert_int(rv, ==, 0);

	return MUNIT_OK;
}
