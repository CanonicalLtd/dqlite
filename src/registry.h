#ifndef REGISTRY_H_
#define REGISTRY_H_

#include <stdbool.h>

#include <sqlite3.h>

#include "lib/queue.h"

#include "db.h"

struct registry
{
	struct config *config;
	queue dbs;
};

void registryInit(struct registry *r, struct config *config);
void registryClose(struct registry *r);

/**
 * Get the db with the given filename. If no one is registered, create one.
 */
int registryDbGet(struct registry *r, const char *filename, struct db **db);

#endif /* REGISTRY_H_*/
