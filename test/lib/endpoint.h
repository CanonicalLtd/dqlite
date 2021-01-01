/* Helpers to create and connect Unix or TCP sockets. */

#ifndef TEST_ENDPOINT_H
#define TEST_ENDPOINT_H

#include <arpa/inet.h>
#include <sys/un.h>

#include "munit.h"

/* A few tests depend on knowing that certain reads and writes will not be short
 * and will happen immediately. */
#define TEST_SOCKET_MIN_BUF_SIZE 4096

/* Munit parameter defining the socket type to use in testEndpointSetup.
 *
 * If set to "unix" a pair of unix abstract sockets will be created. If set to
 * "tcp" a pair of TCP sockets using the loopback interface will be created. */
#define TEST_ENDPOINT_FAMILY "endpoint-family"

/* Null-terminated list of legal values for TEST_ENDPOINT_FAMILY. Currently
 * "unix" and "tcp". */
extern char *testEndpointFamilyValues[];

/* Listening socket endpoint. */
struct testEndpoint
{
	char address[256];  /* Rendered address string. */
	sa_family_t family; /* Address family (either AF_INET or AF_UNIX) */
	int fd;             /* Listening socket. */
	union {             /* Server  address (either a TCP or Unix) */
		struct sockaddr_in inAddress;
		struct sockaddr_un unAddress;
	};
};

/* Create a listening endpoint.
 *
 * This will bind a random address and start listening to it. */
void testEndpointSetup(struct testEndpoint *e, const MunitParameter params[]);

/* Tear down a listening endpoint. */
void testEndpointTearDown(struct testEndpoint *e);

/* Establish a new client connection. */
int testEndpointConnect(struct testEndpoint *e);

/* Accept a new client connection. */
int testEndpointAccept(struct testEndpoint *e);

/* Connect and accept a connection, returning the pair of connected sockets. */
void testEndpointPair(struct testEndpoint *e, int *server, int *client);

/* Return the endpoint address. */
const char *testEndpointAddress(struct testEndpoint *e);

#endif /* TEST_ENDPOINT_H */
