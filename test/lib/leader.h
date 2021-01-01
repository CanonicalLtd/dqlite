/**
 * Setup a test struct leader object.
 */

#ifndef TEST_LEADER_H
#define TEST_LEADER_H

#include "../../src/leader.h"
#include "../../src/registry.h"

#define FIXTURE_LEADER struct leader leader
#define SETUP_LEADER                                              \
	{                                                         \
		struct db *db;                                    \
		int rv;                                           \
		rv = registryDbGet(&f->registry, "test.db", &db); \
		munit_assert_int(rv, ==, 0);                      \
		rv = leaderInit(&f->leader, db, &f->raft);        \
		munit_assert_int(rv, ==, 0);                      \
	}
#define TEAR_DOWN_LEADER leaderClose(&f->leader)

#endif /* TEST_LEADER_H */
