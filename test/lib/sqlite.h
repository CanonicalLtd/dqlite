/* Global SQLite configuration. */

#ifndef TEST_SQLITE_H
#define TEST_SQLITE_H

#include "munit.h"

/* Setup SQLite global state. */
void testSqliteSetup(const MunitParameter params[]);

/* Teardown SQLite global state. */
void testSqliteTearDown(void);

#define SETUP_SQLITE testSqliteSetup(params);
#define TEAR_DOWN_SQLITE testSqliteTearDown();

#endif /* TEST_SQLITE_H */
