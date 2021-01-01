#include <raft.h>
#include <uv.h>

#include <unistd.h>

#include "../../../src/lib/transport.h"
#include "../../lib/endpoint.h"
#include "../../lib/runner.h"
#include "../../lib/uv.h"

TEST_MODULE(extUv);

/******************************************************************************
 *
 * Helpers
 *
 ******************************************************************************/

struct fixture
{
	struct uv_loop_s loop;
	struct uv_stream_s *listener;
	struct testEndpoint endpoint;
	int client;
	union {
		uv_tcp_t tcp;
		uv_pipe_t pipe;
		uv_stream_t stream;
	};
};

/* Return a buffer of size TEST_SOCKET_MIN_BUF_SIZE */
static uv_buf_t *bufMalloc(void)
{
	uv_buf_t *buf = munit_malloc(sizeof *buf);
	buf->base = munit_malloc(TEST_SOCKET_MIN_BUF_SIZE);
	buf->len = TEST_SOCKET_MIN_BUF_SIZE;
	return buf;
}

/* Free the buffer returned by bufMalloc() */
static void bufFree(uv_buf_t *buf)
{
	free(buf->base);
	free(buf);
}

/******************************************************************************
 *
 * Parameters
 *
 ******************************************************************************/

/* Run the tests using both TCP and Unix sockets. */
static MunitParameterEnum endpointParams[] = {
    {TEST_ENDPOINT_FAMILY, testEndpointFamilyValues},
    {NULL, NULL},
};

/******************************************************************************
 *
 * Setup and tear down
 *
 ******************************************************************************/

static void listenCb(uv_stream_t *listener, int status)
{
	struct fixture *f = listener->data;
	int rv;
	munit_assert_int(status, ==, 0);

	switch (listener->type) {
		case UV_TCP:
			rv = uv_tcp_init(&f->loop, &f->tcp);
			munit_assert_int(rv, ==, 0);
			break;
		case UV_NAMED_PIPE:
			rv = uv_pipe_init(&f->loop, &f->pipe, 0);
			munit_assert_int(rv, ==, 0);
			break;
		default:
			munit_assert(0);
	}

	rv = uv_accept(listener, &f->stream);
	munit_assert_int(rv, ==, 0);
}

static void *setup(const MunitParameter params[], void *userData)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	(void)userData;

	testUvSetup(params, &f->loop);
	testEndpointSetup(&f->endpoint, params);

	rv = transportStream(&f->loop, f->endpoint.fd, &f->listener);
	munit_assert_int(rv, ==, 0);

	f->listener->data = f;

	rv = uv_listen(f->listener, 128, listenCb);
	munit_assert_int(rv, ==, 0);

	f->client = testEndpointConnect(&f->endpoint);

	testUvRun(&f->loop, 1);

	return f;
}

static void tear_down(void *data)
{
	struct fixture *f = data;
	int rv;
	rv = close(f->client);
	munit_assert_int(rv, ==, 0);
	uv_close((struct uv_handle_s *)f->listener, (uv_close_cb)raft_free);
	testEndpointTearDown(&f->endpoint);
	uv_close((uv_handle_t *)(&f->stream), NULL);
	testUvStop(&f->loop);
	testUvTearDown(&f->loop);
	free(f);
}

/******************************************************************************
 *
 * uv_write
 *
 ******************************************************************************/

TEST_SUITE(write);
TEST_SETUP(write, setup);
TEST_TEAR_DOWN(write, tear_down);

/* Writing an amount of data below the buffer size makes that data immediately
 * available for reading. */
TEST_CASE(write, sync, endpointParams)
{
	struct fixture *f = data;
	uv_write_t req;
	uv_buf_t *buf1 = bufMalloc();
	uv_buf_t *buf2 = bufMalloc();
	int rv;

	(void)params;

	rv = uv_write(&req, &f->stream, buf1, 1, NULL);
	munit_assert_int(rv, ==, 0);

	rv = read(f->client, buf2->base, buf2->len);
	munit_assert_int(rv, ==, buf2->len);

	testUvRun(&f->loop, 1);

	bufFree(buf1);
	bufFree(buf2);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * uvRead
 *
 ******************************************************************************/

TEST_SUITE(read);
TEST_SETUP(read, setup);
TEST_TEAR_DOWN(read, tear_down);

static void testReadSyncAllocCb(uv_handle_t *stream, size_t _, uv_buf_t *buf)
{
	(void)stream;
	(void)_;

	buf->len = TEST_SOCKET_MIN_BUF_SIZE;
	buf->base = munit_malloc(TEST_SOCKET_MIN_BUF_SIZE);
}

static void testReadSyncReadCb(uv_stream_t *stream,
			       ssize_t nread,
			       const uv_buf_t *buf)
{
	bool *readCbCalled;

	/* Apprently there's an empty read before the actual one. */
	if (nread == 0) {
		free(buf->base);
		return;
	}

	munit_assert_int(nread, ==, TEST_SOCKET_MIN_BUF_SIZE);
	munit_assert_int(buf->len, ==, TEST_SOCKET_MIN_BUF_SIZE);

	readCbCalled = stream->data;

	*readCbCalled = true;

	free(buf->base);
}

/* Reading an amount of data below the buffer happens synchronously. */
TEST_CASE(read, sync, endpointParams)
{
	struct fixture *f = data;
	uv_buf_t *buf = bufMalloc();
	int rv;
	bool readCbCalled;

	(void)params;

	f->stream.data = &readCbCalled;

	rv = uv_read_start(&f->stream, testReadSyncAllocCb, testReadSyncReadCb);

	rv = write(f->client, buf->base, buf->len);
	munit_assert_int(rv, ==, buf->len);

	testUvRun(&f->loop, 1);

	munit_assert_true(readCbCalled);

	bufFree(buf);

	return MUNIT_OK;
}
