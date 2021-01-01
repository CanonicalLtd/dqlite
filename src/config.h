#ifndef CONFIG_H_
#define CONFIG_H_

#include "logger.h"

/**
 * Value object holding dqlite configuration.
 */
struct config
{
	dqlite_node_id id;             /* Unique instance ID */
	char *address;                 /* Instance address */
	unsigned heartbeatTimeout;     /* In milliseconds */
	unsigned pageSize;             /* Database page size */
	unsigned checkpointThreshold;  /* In outstanding WAL frames */
	struct logger logger;          /* Custom logger */
	char name[256];                /* VFS/replication registriatio name */
	unsigned long long failureDomain;  /* User-provided failure domain */
	unsigned long long int weight;     /* User-provided node weight */
};

/**
 * Initialize the config object with required values and set the rest to sane
 * defaults. A copy will be made of the given @address.
 */
int configInit(struct config *c, dqlite_node_id id, const char *address);

/**
 * Release any memory held by the config object.
 */
void configClose(struct config *c);

#endif /* DQLITE_OPTIONS_H */
