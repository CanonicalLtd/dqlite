#include "../../src/request.h"

#include "../lib/heap.h"
#include "../lib/runner.h"

TEST_MODULE(request);

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture
{
	void *buf;
};

static void *setup(const MunitParameter params[], void *userData)
{
	struct fixture *f;
	f = munit_malloc(sizeof *f);
	SETUP_HEAP;
	f->buf = NULL;
	return f;
}

static void tear_down(void *data)
{
	struct fixture *f = data;
	free(f->buf);
	TEAR_DOWN_HEAP;
	free(f);
}

/******************************************************************************
 *
 * Helper macros
 *
 ******************************************************************************/

#define ALLOC_BUF(N) f->buf = munit_malloc(N);

/******************************************************************************
 *
 * Serialize
 *
 ******************************************************************************/

TEST_SUITE(serialize);
TEST_SETUP(serialize, setup);
TEST_TEAR_DOWN(serialize, tear_down);

TEST_CASE(serialize, leader, NULL)
{
	struct fixture *f = data;
	struct requestleader request;
	void *cursor1;
	struct cursor cursor2;
	size_t n = requestleaderSizeof(&request);
	(void)params;
	ALLOC_BUF(n);
	cursor1 = f->buf;
	requestleaderEncode(&request, &cursor1);
	cursor2.p = f->buf;
	cursor2.cap = n;
	requestleaderDecode(&cursor2, &request);
	return MUNIT_OK;
}

/******************************************************************************
 *
 * Decode
 *
 ******************************************************************************/

TEST_SUITE(decode);
TEST_SETUP(decode, setup);
TEST_TEAR_DOWN(decode, tear_down);

TEST_CASE(decode, leader, NULL)
{
	(void)data;
	(void)params;
	return MUNIT_OK;
}

#if 0
TEST_CASE(decode, client, NULL)
{
	struct request *request = data;
	int err;

	(void)params;

	testMessageSendClient(123, &request->message);

	err = requestDecode(request);
	munit_assert_int(err, ==, 0);

	munit_assert_int(request->client.id, ==, 123);

	return MUNIT_OK;
}

TEST_CASE(decode, heartbeat, NULL)
{
	struct request *request = data;
	int err;

	(void)params;

	testMessageSendHeartbeat(666, &request->message);

	err = requestDecode(request);
	munit_assert_int(err, ==, 0);

	munit_assert_int(request->heartbeat.timestamp, ==, 666);

	return MUNIT_OK;
}

TEST_CASE(decode, open, NULL)
{
	struct request *request = data;
	int err;

	(void)params;

	testMessageSendOpen("test.db", 123, "volatile", &request->message);

	err = requestDecode(request);
	munit_assert_int(err, ==, 0);

	munit_assert_string_equal(request->open.name, "test.db");
	munit_assert_int(request->open.flags, ==, 123);
	munit_assert_string_equal(request->open.vfs, "volatile");

	return MUNIT_OK;
}
#endif
