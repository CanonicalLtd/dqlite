/**
 * State of a single database.
 */

#ifndef DB_H_
#define DB_H_

#include "lib/queue.h"

#include "config.h"

struct db
{
	struct config *config; /* Dqlite configuration */
	char *filename;        /* Database filename */
	sqlite3 *follower;     /* Follower connection */
	queue leaders;         /* Open leader connections */
	unsigned txId;         /* Current ongoing transaction ID, if any */
	queue queue;           /* Prev/next database, used by the registry */
};

/**
 * Initialize a database object.
 *
 * The given @filename will be copied.
 */
void dbInit(struct db *db, struct config *config, const char *filename);

/**
 * Release all memory associated with a database object.
 *
 * If the follower connection was opened, it will be closed.
 */
void dbClose(struct db *db);

/**
 * Open the follower connection associated with this database.
 */
int dbOpenFollower(struct db *db);

#endif /* DB_H_*/
