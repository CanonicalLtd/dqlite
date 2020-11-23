#include <stdlib.h>

#include "../../../src/lib/registry.h"

#include "../../lib/runner.h"

TEST_MODULE(libRegistry);

struct testItem
{
	size_t id;
	int *ptr;
};

static void testItem_init(struct testItem *i)
{
	munit_assert(i != NULL);

	i->ptr = (int *)sqlite3_malloc(sizeof(*(i->ptr)));
	*i->ptr = 123;
}

static void testItem_close(struct testItem *i)
{
	munit_assert(i != NULL);
	munit_assert(i->ptr != NULL);

	sqlite3_free(i->ptr);
}

static const char *testItem_hash(struct testItem *i)
{
	munit_assert(i != NULL);

	return "x";
}

REGISTRY(test_registry, testItem);
REGISTRY_METHODS(test_registry, testItem);

static void *setup(const MunitParameter params[], void *user_data)
{
	struct test_registry *registry;

	(void)params;
	(void)user_data;

	registry = (struct test_registry *)munit_malloc(sizeof(*registry));

	test_registry_init(registry);

	return registry;
}

static void tear_down(void *data)
{
	struct test_registry *registry = data;

	test_registry_close(registry);
	free(registry);
}

TEST_SUITE(add);
TEST_SETUP(add, setup);
TEST_TEAR_DOWN(add, tear_down);

static char *testAddN[] = {"1", "2", "3", "5", "6", "7", "8", "9", "10", NULL};

static MunitParameterEnum testAddParams[] = {
    {"n", testAddN},
    {NULL, NULL},
};

/* Add N items. */
TEST_CASE(add, basic, testAddParams)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item;
	int n;
	int i;

	n = atoi(munit_parameters_get(params, "n"));
	munit_assert_int(n, >, 0);

	for (i = 0; i < n; i++) {
		err = test_registry_add(registry, &item);
		munit_assert_int(err, ==, 0);

		munit_assert_ptr_not_equal(item, NULL);
		munit_assert_ptr_not_equal(item->ptr, NULL);
		munit_assert_int(123, ==, *item->ptr);
	}

	return MUNIT_OK;
}

/* Add three items, delete the second, and then add another one. The original ID
 * of the deleted item gets reused. */
TEST_CASE(add, delAdd, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item1;
	struct testItem *item2;
	struct testItem *item3;
	struct testItem *item4;
	int item2Id;

	(void)params;

	err = test_registry_add(registry, &item1);
	munit_assert_int(err, ==, 0);

	err = test_registry_add(registry, &item2);
	munit_assert_int(err, ==, 0);
	item2Id = item2->id;

	err = test_registry_add(registry, &item3);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, item2);
	munit_assert_int(err, ==, 0);

	err = test_registry_add(registry, &item4);
	munit_assert_int(err, ==, 0);

	munit_assert_int(item4->id, ==, item2Id);

	return MUNIT_OK;
}

/* Add N items and then delete them all. */
TEST_CASE(add, andDel, testAddParams)
{
	struct test_registry *registry = data;
	int err;
	struct testItem **items;
	int n;
	int i;

	n = atoi(munit_parameters_get(params, "n"));
	munit_assert_int(n, >, 0);

	items = munit_malloc(n * sizeof(*items));

	for (i = 0; i < n; i++) {
		err = test_registry_add(registry, &items[i]);
		munit_assert_int(err, ==, 0);
	}

	for (i = 0; i < n; i++) {
		err = test_registry_del(registry, items[i]);
		munit_assert_int(err, ==, 0);
	}

	free(items);

	return MUNIT_OK;
}

TEST_SUITE(get);
TEST_SETUP(get, setup);
TEST_TEAR_DOWN(get, tear_down);

/* Retrieve a previously added item. */
TEST_CASE(get, basic, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item;

	(void)params;

	err = test_registry_add(registry, &item);
	munit_assert_int(err, ==, 0);

	munit_assert_ptr_equal(test_registry_get(registry, item->id), item);

	return MUNIT_OK;
}

/* An item gets added and then deleted. Trying to fetch the item using its
 * former ID results in a NULL pointer. */
TEST_CASE(get, deleted, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item;
	size_t id;

	(void)params;

	err = test_registry_add(registry, &item);
	munit_assert_int(err, ==, 0);

	id = item->id;

	err = test_registry_del(registry, item);
	munit_assert_int(err, ==, 0);

	munit_assert_ptr_equal(test_registry_get(registry, id), NULL);

	return MUNIT_OK;
}

/* Retrieve an item with an ID bigger than the current registry's length. */
TEST_CASE(get, outOfBound, NULL)
{
	struct test_registry *registry = data;
	struct testItem *item = test_registry_get(registry, 123);

	(void)params;

	munit_assert_ptr_equal(item, NULL);

	return MUNIT_OK;
}

TEST_SUITE(idx);
TEST_SETUP(idx, setup);
TEST_TEAR_DOWN(idx, tear_down);

/* Find the index of a matching item. */
TEST_CASE(idx, found, NULL)
{
	struct test_registry *registry = data;
	struct testItem *item;
	size_t i;
	int err;

	(void)params;

	err = test_registry_add(registry, &item);
	munit_assert_int(err, ==, 0);

	err = test_registry_idx(registry, "x", &i);
	munit_assert_int(err, ==, 0);

	munit_assert_int(i, ==, item->id);

	return MUNIT_OK;
}

/* No matching item. */
TEST_CASE(idx, notFound, NULL)
{
	struct test_registry *registry = data;
	struct testItem *item1;
	struct testItem *item2;
	size_t i;
	int err;

	(void)params;

	err = test_registry_add(registry, &item1);
	munit_assert_int(err, ==, 0);

	err = test_registry_add(registry, &item2);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, item1);
	munit_assert_int(err, ==, 0);

	err = test_registry_idx(registry, "y", &i);
	munit_assert_int(err, ==, DQLITE_NOTFOUND);

	return MUNIT_OK;
}

TEST_SUITE(del);
TEST_SETUP(del, setup);
TEST_TEAR_DOWN(del, tear_down);

/* Delete an item from the registry. */
TEST_CASE(del, basic, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item;

	(void)params;

	err = test_registry_add(registry, &item);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, item);
	munit_assert_int(err, ==, 0);

	return MUNIT_OK;
}

/* Deleting an item twice results in an error. */
TEST_CASE(del, twice, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item;
	struct testItem itemClone;

	(void)params;

	err = test_registry_add(registry, &item);
	munit_assert_int(err, ==, 0);
	itemClone.id = item->id;

	err = test_registry_del(registry, item);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, &itemClone);
	munit_assert_int(err, ==, DQLITE_NOTFOUND);

	return MUNIT_OK;
}

/* Deleting an item twice results in an error, also if the item being deleted
 * again has an ID lower than the highest one. */
TEST_CASE(del, twice_middle, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item1;
	struct testItem *item2;
	struct testItem item1Clone;

	(void)params;

	err = test_registry_add(registry, &item1);
	munit_assert_int(err, ==, 0);
	item1Clone.id = item1->id;

	err = test_registry_add(registry, &item2);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, item1);
	munit_assert_int(err, ==, 0);

	err = test_registry_del(registry, &item1Clone);
	munit_assert_int(err, ==, DQLITE_NOTFOUND);

	return MUNIT_OK;
}

/* Deleting an item with an unknown ID results in an error. */
TEST_CASE(del, outOfBounds, NULL)
{
	struct test_registry *registry = data;
	struct testItem item;
	int err;

	(void)params;

	item.id = 123;

	err = test_registry_del(registry, &item);

	munit_assert_int(err, ==, DQLITE_NOTFOUND);

	return MUNIT_OK;
}

/* Add several items and then delete them. */
TEST_CASE(del, many, NULL)
{
	struct test_registry *registry = data;
	int err;
	struct testItem *item1;
	struct testItem *item2;
	struct testItem *item3;

	(void)params;

	err = test_registry_add(registry, &item1);
	munit_assert_int(err, ==, 0);

	munit_assert_int(item1->id, ==, 0);

	err = test_registry_add(registry, &item2);
	munit_assert_int(err, ==, 0);

	munit_assert_int(item2->id, ==, 1);

	err = test_registry_add(registry, &item3);
	munit_assert_int(err, ==, 0);

	munit_assert_int(item3->id, ==, 2);

	munit_assert_int(3, ==, registry->len);
	munit_assert_int(4, ==, registry->cap);

	err = test_registry_del(registry, item3);
	munit_assert_int(err, ==, 0);

	munit_assert_int(2, ==, registry->len);
	munit_assert_int(4, ==, registry->cap);

	err = test_registry_del(registry, item2);
	munit_assert_int(err, ==, 0);

	munit_assert_int(1, ==, registry->len);
	munit_assert_int(2, ==, registry->cap);

	return MUNIT_OK;
}
