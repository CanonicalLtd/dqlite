#include <string.h>

#include "../include/dqlite.h"

#include "./lib/assert.h"

#include "db.h"

/* Open a SQLite connection and set it to follower mode. */
static int openFollowerConn(const char *filename,
			    const char *vfs,
			    unsigned pageSize,
			    sqlite3 **conn);

void dbInit(struct db *db, struct config *config, const char *filename)
{
	db->config = config;
	db->filename = sqlite3_malloc((int)(strlen(filename) + 1));
	assert(db->filename != NULL); /* TODO: return an error instead */
	strcpy(db->filename, filename);
	db->follower = NULL;
	db->txId = 0;
	QUEUE_INIT(&db->leaders);
}

void dbClose(struct db *db)
{
	assert(QUEUE_IS_EMPTY(&db->leaders));
	if (db->follower != NULL) {
		int rc;
		rc = sqlite3_close(db->follower);
		assert(rc == SQLITE_OK);
	}
	sqlite3_free(db->filename);
}

int dbOpenFollower(struct db *db)
{
	int rc;
	assert(db->follower == NULL);
	rc = openFollowerConn(db->filename, db->config->name,
			      db->config->pageSize, &db->follower);
	if (rc != 0) {
		return rc;
	}
	return 0;
}

static int openFollowerConn(const char *filename,
			    const char *vfs,
			    unsigned pageSize,
			    sqlite3 **conn)
{
	char pragma[255];
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	char *msg = NULL;
	int rc;

	rc = sqlite3_open_v2(filename, conn, flags, vfs);
	if (rc != SQLITE_OK) {
		goto err;
	}

	/* Enable extended result codes */
	rc = sqlite3_extended_result_codes(*conn, 1);
	if (rc != SQLITE_OK) {
		goto errAfterOpen;
	}

	/* Set the page size. */
	sprintf(pragma, "PRAGMA page_size=%d", pageSize);
	rc = sqlite3_exec(*conn, pragma, NULL, NULL, &msg);
	if (rc != SQLITE_OK) {
		goto errAfterOpen;
	}

	/* Disable syncs. */
	rc = sqlite3_exec(*conn, "PRAGMA synchronous=OFF", NULL, NULL, &msg);
	if (rc != SQLITE_OK) {
		goto errAfterOpen;
	}

	/* Set WAL journaling. */
	rc = sqlite3_exec(*conn, "PRAGMA journal_mode=WAL", NULL, NULL, &msg);
	if (rc != SQLITE_OK) {
		goto errAfterOpen;
	}

	rc =
	    sqlite3_db_config(*conn, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, NULL);
	if (rc != SQLITE_OK) {
		goto errAfterOpen;
	}

	return 0;

errAfterOpen:
	sqlite3_close(*conn);
err:
	if (msg != NULL) {
		sqlite3_free(msg);
	}
	return rc;
}
