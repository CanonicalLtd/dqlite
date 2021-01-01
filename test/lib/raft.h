/**
 * Helpers for setting up a standalone raft instance with a libuv transport.
 */

#ifndef TEST_RAFT_H
#define TEST_RAFT_H

#include <raft.h>
#include <raft/uv.h>
#include <uv.h>

#include "../../src/fsm.h"
#include "../../src/transport.h"
#include "fs.h"
#include "logger.h"
#include "munit.h"
#include "uv.h"

#define FIXTURE_RAFT                            \
	char *dir;                              \
	struct uv_loop_s loop;                  \
	struct raft_uv_transport raftTransport; \
	struct raft_io raftIo;                  \
	struct raft_fsm fsm;                    \
	struct raft raft

#define SETUP_RAFT                                                      \
	{                                                               \
		int rv2;                                                \
		f->dir = testDirSetup();                                \
		testUvSetup(params, &f->loop);                          \
		rv2 = raftProxyInit(&f->raftTransport, &f->loop);       \
		munit_assert_int(rv2, ==, 0);                           \
		rv2 = raft_uv_init(&f->raftIo, &f->loop, f->dir,        \
				   &f->raftTransport);                  \
		munit_assert_int(rv2, ==, 0);                           \
		rv2 = fsmInit(&f->fsm, &f->config, &f->registry);       \
		munit_assert_int(rv2, ==, 0);                           \
		rv2 = raft_init(&f->raft, &f->raftIo, &f->fsm, 1, "1"); \
		munit_assert_int(rv2, ==, 0);                           \
	}

#define TEAR_DOWN_RAFT                             \
	{                                          \
		raft_close(&f->raft, NULL);        \
		testUvStop(&f->loop);              \
		raft_uv_close(&f->raftIo);         \
		fsmClose(&f->fsm);                 \
		testUvTearDown(&f->loop);          \
		raftProxyClose(&f->raftTransport); \
		testDirTearDown(f->dir);           \
	}

/**
 * Bootstrap the fixture raft instance with a configuration containing only
 * itself.
 */
#define RAFT_BOOTSTRAP                                                      \
	{                                                                   \
		struct raft_configuration configuration;                    \
		int rv2;                                                    \
		raft_configuration_init(&configuration);                    \
		rv2 = raft_configuration_add(&configuration, 1, "1", true); \
		munit_assert_int(rv2, ==, 0);                               \
		rv2 = raft_bootstrap(&f->raft, &configuration);             \
		munit_assert_int(rv2, ==, 0);                               \
		raft_configuration_close(&configuration);                   \
	}

#define RAFT_START                            \
	{                                     \
		int rv2;                      \
		rv2 = raft_start(&f->raft);   \
		munit_assert_int(rv2, ==, 0); \
	}

#endif /* TEST_RAFT_H */
