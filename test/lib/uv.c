#include "uv.h"

#define TEST_UV_MAX_LOOP_RUN 10 /* Max n. of loop iterations upon teardown */

void testUvSetup(const MunitParameter params[], struct uv_loop_s *l)
{
    int rv;

    (void)params;

    rv = uv_loop_init(l);
    munit_assert_int(rv, ==, 0);
}

int testUvRun(struct uv_loop_s *l, unsigned n)
{
    unsigned i;
    int rv;

    munit_assert_int(n, >, 0);

    for (i = 0; i < n; i++) {
        rv = uv_run(l, UV_RUN_ONCE);
        if (rv < 0) {
            munit_errorf("uv_run: %s (%d)", uv_strerror(rv), rv);
        }
        if (rv == 0) {
            break;
        }
    }

    return rv;
}

void testUvStop(struct uv_loop_s *l)
{
	unsigned nHandles;

	/* Spin a few times to trigger pending callbacks. */
	nHandles = testUvRun(l, TEST_UV_MAX_LOOP_RUN);
	if (nHandles > 0) {
		munit_errorf("loop has still %d pending active handles",
			     nHandles);
	}
}

static void testUvWalkCb(uv_handle_t *handle, void *arg)
{
    (void)arg;

    munit_logf(MUNIT_LOG_INFO, "handle %d", handle->type);
}

void testUvTearDown(struct uv_loop_s *l)
{
    int rv;

    rv = uv_loop_close(l);
    if (rv != 0) {
	    uv_walk(l, testUvWalkCb, NULL);
	    munit_errorf("uv_loop_close: %s (%d)", uv_strerror(rv), rv);
    }

    rv = uv_replace_allocator(malloc, realloc, calloc, free);
    munit_assert_int(rv, ==, 0);
}
