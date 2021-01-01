#include "../../../src/lib/buffer.h"

#include "../../lib/runner.h"

TEST_MODULE(libBuffer);

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture
{
	struct buffer buffer;
};

static void *setup(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rc;
	(void)params;
	(void)userData;
	rc = bufferInit(&f->buffer);
	munit_assert_int(rc, ==, 0);
	return f;
}

static void tear_down(void *data)
{
	struct fixture *f = data;
	bufferClose(&f->buffer);
	free(f);
}

/******************************************************************************
 *
 * Helper macros.
 *
 ******************************************************************************/

#define ADVANCE(SIZE)                                     \
	{                                                 \
		cursor = bufferAdvance(&f->buffer, SIZE); \
		munit_assert_ptr_not_null(cursor);        \
	}

/******************************************************************************
 *
 * Assertions.
 *
 ******************************************************************************/

#define ASSERT_N_PAGES(N) munit_assert_int(f->buffer.nPages, ==, N)

/******************************************************************************
 *
 * bufferInit
 *
 ******************************************************************************/

TEST_SUITE(init);
TEST_SETUP(init, setup);
TEST_TEAR_DOWN(init, tear_down);

/* If n is 0, then the prefix is used to dermine the number of elements of the
 * tuple. */
TEST_CASE(init, nPages, NULL)
{
	struct fixture *f = data;
	(void)params;
	ASSERT_N_PAGES(1);
	munit_assert_int(f->buffer.pageSize, ==, 4096);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * bufferAdvance
 *
 ******************************************************************************/

TEST_SUITE(advance);
TEST_SETUP(advance, setup);
TEST_TEAR_DOWN(advance, tear_down);

/* The buffer already has enough capacity. */
TEST_CASE(advance, enough, NULL)
{
	struct fixture *f = data;
	void *cursor;
	(void)params;
	ADVANCE(16);
	ASSERT_N_PAGES(1);
	return MUNIT_OK;
}

/* The buffer needs to double its size once. */
TEST_CASE(advance, double, NULL)
{
	struct fixture *f = data;
	void *cursor;
	(void)params;
	ADVANCE(16 + f->buffer.pageSize);
	ASSERT_N_PAGES(2);
	return MUNIT_OK;
}

/* The buffer needs to double its sice twice. */
TEST_CASE(advance, doubleTwice, NULL)
{
	struct fixture *f = data;
	void *cursor;
	(void)params;
	ADVANCE(16 + 3 * f->buffer.pageSize);
	ASSERT_N_PAGES(4);
	return MUNIT_OK;
}
