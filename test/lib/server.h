/* Setup fully blown servers running in standalone threads. */

#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <pthread.h>
#include <sys/un.h>

#include "../../src/client.h"

#include "../../include/dqlite.h"

#include "endpoint.h"
#include "munit.h"

struct testServer
{
	unsigned id;                   /* Server ID. */
	char address[8];               /* Server address. */
	char *dir;                     /* Data directory. */
	dqlite_node *dqlite;           /* Dqlite instance. */
	struct client client;          /* Connected client. */
	struct testServer *others[5];  /* Other servers, by ID-1. */
};

/* Initialize the test server. */
void testServerSetup(struct testServer *s,
		     unsigned id,
		     const MunitParameter params[]);

/* Cleanup the test server. */
void testServerTearDown(struct testServer *s);

/* Start the test server. */
void testServerStart(struct testServer *s);

/* Connect all the given the servers to each other. */
void testServerNetwork(struct testServer *servers, unsigned nServers);

/* Return a client connected to the server. */
struct client *testServerClient(struct testServer *s);

#endif /* TEST_SERVER_H */
