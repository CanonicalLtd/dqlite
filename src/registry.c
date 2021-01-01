#include <string.h>

#include "../include/dqlite.h"

#include "lib/assert.h"

#include "registry.h"

void registryInit(struct registry *r, struct config *config)
{
	r->config = config;
	QUEUE_INIT(&r->dbs);
}

void registryClose(struct registry *r)
{
	while (!QUEUE_IS_EMPTY(&r->dbs)) {
		struct db *db;
		queue *head;
		head = QUEUE_HEAD(&r->dbs);
		QUEUE_REMOVE(head);
		db = QUEUE_DATA(head, struct db, queue);
		dbClose(db);
		sqlite3_free(db);
	}
}

int registryDbGet(struct registry *r, const char *filename, struct db **db)
{
	queue *head;
	QUEUE_FOREACH(head, &r->dbs)
	{
		*db = QUEUE_DATA(head, struct db, queue);
		if (strcmp((*db)->filename, filename) == 0) {
			return 0;
		}
	}
	*db = sqlite3_malloc(sizeof **db);
	if (*db == NULL) {
		return DQLITE_NOMEM;
	}
	dbInit(*db, r->config, filename);
	QUEUE_PUSH(&r->dbs, &(*db)->queue);
	return 0;
}
