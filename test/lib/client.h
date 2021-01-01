/* Setup a test dqlite client. */

#include "endpoint.h"

#ifndef TEST_CLIENT_H
#define TEST_CLIENT_H

#define FIXTURE_CLIENT                \
	struct client client;         \
	struct testEndpoint endpoint; \
	int server

#define SETUP_CLIENT                                                  \
	{                                                             \
		int _rv;                                              \
		int _client;                                          \
		testEndpointSetup(&f->endpoint, params);              \
		_rv = listen(f->endpoint.fd, 16);                     \
		munit_assert_int(_rv, ==, 0);                         \
		testEndpointPair(&f->endpoint, &f->server, &_client); \
		clientInit(&f->client, _client);                      \
	}

#define TEAR_DOWN_CLIENT         \
	clientClose(&f->client); \
	testEndpointTearDown(&f->endpoint)

#endif /* TEST_CLIENT_H */
