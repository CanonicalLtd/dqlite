#include <sqlite3.h>

#include "sqlite.h"

void testSqliteSetup(const MunitParameter params[])
{
	int rc;
	(void)params;
	rc = sqlite3_shutdown();
	if (rc != SQLITE_OK) {
		munit_errorf("sqlite_init(): %s", sqlite3_errstr(rc));
	}
}

void testSqliteTearDown()
{
	int rc;
	rc = sqlite3_shutdown();
	if (rc != SQLITE_OK) {
		munit_errorf("sqlite_shutdown(): %s", sqlite3_errstr(rc));
	}
}
