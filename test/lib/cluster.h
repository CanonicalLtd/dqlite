/**
 * Helpers to setup a raft cluster in test fixtures.
 *
 * Each raft instance will use its own dqlite FSM, which in turn will be created
 * using its own config, registry and logger.
 *
 * The fixture will also register a VFS and a SQLite replication object for each
 * raft instance, using "test<i>" as registration name, where <i> is the raft
 * instance index.
 *
 * This fixture is meant to be used as base-line fixture for most higher-level
 * tests.
 */

#ifndef TEST_CLUSTER_H
#define TEST_CLUSTER_H

#include <raft.h>
#include <raft/fixture.h>

#include "../../src/config.h"
#include "../../src/fsm.h"
#include "../../src/registry.h"
#include "../../src/vfs.h"

#include "../lib/heap.h"
#include "../lib/logger.h"
#include "../lib/sqlite.h"

#define N_SERVERS 3

#define V1 0
#define V2 1

struct server
{
	struct logger logger;
	struct config config;
	sqlite3_vfs vfs;
	struct registry registry;
};

#define FIXTURE_CLUSTER                   \
	struct server servers[N_SERVERS]; \
	struct raft_fsm fsms[N_SERVERS];  \
	struct raft_fixture cluster;

#define SETUP_CLUSTER(VERSION)                                              \
	{                                                                   \
		struct raft_configuration _configuration;                   \
		unsigned _i;                                                \
		int _rv;                                                    \
		SETUP_HEAP;                                                 \
		SETUP_SQLITE;                                               \
		_rv = raft_fixture_init(&f->cluster, N_SERVERS, f->fsms);   \
		munit_assert_int(_rv, ==, 0);                               \
		for (_i = 0; _i < N_SERVERS; _i++) {                        \
			SETUP_SERVER(_i, VERSION);                          \
		}                                                           \
		_rv = raft_fixture_configuration(&f->cluster, N_SERVERS,    \
						 &_configuration);          \
		munit_assert_int(_rv, ==, 0);                               \
		_rv = raft_fixture_bootstrap(&f->cluster, &_configuration); \
		munit_assert_int(_rv, ==, 0);                               \
		raft_configuration_close(&_configuration);                  \
		_rv = raft_fixture_start(&f->cluster);                      \
		munit_assert_int(_rv, ==, 0);                               \
	}

#define SETUP_SERVER(I, VERSION)                                 \
	{                                                        \
		struct server *_s = &f->servers[I];              \
		struct raft_fsm *_fsm = &f->fsms[I];             \
		char address[16];                                \
		int _rc;                                         \
                                                                 \
		testLoggerSetup(params, &_s->logger);            \
                                                                 \
		sprintf(address, "%d", I + 1);                   \
                                                                 \
		_rc = configInit(&_s->config, I + 1, address);   \
		munit_assert_int(_rc, ==, 0);                    \
                                                                 \
		registryInit(&_s->registry, &_s->config);        \
                                                                 \
		_rc = VfsInit(&_s->vfs, _s->config.name);        \
		munit_assert_int(_rc, ==, 0);                    \
		_rc = sqlite3_vfs_register(&_s->vfs, 0);         \
		munit_assert_int(_rc, ==, 0);                    \
                                                                 \
		_rc = fsmInit(_fsm, &_s->config, &_s->registry); \
		munit_assert_int(_rc, ==, 0);                    \
	}

#define TEAR_DOWN_CLUSTER                            \
	{                                            \
		int _i;                              \
		for (_i = 0; _i < N_SERVERS; _i++) { \
			TEAR_DOWN_SERVER(_i);        \
		}                                    \
		raft_fixture_close(&f->cluster);     \
		TEAR_DOWN_SQLITE;                    \
		TEAR_DOWN_HEAP;                      \
	}

#define TEAR_DOWN_SERVER(I)                         \
	{                                           \
		struct server *s = &f->servers[I];  \
		struct raft_fsm *fsm = &f->fsms[I]; \
		fsmClose(fsm);                      \
		registryClose(&s->registry);        \
		sqlite3_vfs_unregister(&s->vfs);    \
		VfsClose(&s->vfs);                  \
		configClose(&s->config);            \
		testLoggerTearDown(&s->logger);     \
	}

#define CLUSTER_CONFIG(I) &f->servers[I].config
#define CLUSTER_LOGGER(I) &f->servers[I].logger
#define CLUSTER_LEADER(I) &f->servers[I].leader
#define CLUSTER_REGISTRY(I) &f->servers[I].registry
#define CLUSTER_RAFT(I) raft_fixture_get(&f->cluster, I)
#define CLUSTER_LAST_INDEX(I) raft_last_index(CLUSTER_RAFT(I))
#define CLUSTER_DISCONNECT(I, J) raft_fixture_disconnect(&f->cluster, I, J)
#define CLUSTER_RECONNECT(I, J) raft_fixture_reconnect(&f->cluster, I, J)

#define CLUSTER_ELECT(I) raft_fixture_elect(&f->cluster, I)
#define CLUSTER_DEPOSE raft_fixture_depose(&f->cluster)
#define CLUSTER_APPLIED(N)                                                   \
	{                                                                    \
		int _i;                                                      \
		for (_i = 0; _i < N_SERVERS; _i++) {                         \
			bool done;                                           \
			done = raft_fixture_step_until_applied(&f->cluster,  \
							       _i, N, 1000); \
			munit_assert_true(done);                             \
		}                                                            \
	}

#define CLUSTER_STEP raft_fixture_step(&f->cluster)

#define CLUSTER_SNAPSHOT_THRESHOLD(I, N) \
	raft_set_snapshot_threshold(CLUSTER_RAFT(I), N)
#define CLUSTER_SNAPSHOT_TRAILING(I, N) \
	raft_set_snapshot_trailing(CLUSTER_RAFT(I), N)

#endif /* TEST_CLUSTER_H */
