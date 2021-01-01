#include "unistd.h"

#include "fs.h"
#include "server.h"

static int endpointConnect(void *data,
			   const char *address,
			   int *fd)
{
	struct sockaddr_un addr;
	int rv;
	(void)address;
	(void)data;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path + 1, address + 1);
	*fd = socket(AF_UNIX, SOCK_STREAM, 0);
	munit_assert_int(*fd, !=, -1);
	rv = connect(*fd, (struct sockaddr *)&addr, sizeof(sa_family_t) + strlen(address + 1) + 1);
	munit_assert_int(rv, ==, 0);
	return 0;
}

void testServerSetup(struct testServer *s,
		     const unsigned id,
		     const MunitParameter params[])
{
	(void)params;

	s->id = id;
	sprintf(s->address, "@%u", id);

	s->dir = testDirSetup();

	memset(s->others, 0, sizeof s->others);
}

void testServerTearDown(struct testServer *s)
{
	int rv;

	clientClose(&s->client);
	close(s->client.fd);
	rv = dqlite_node_stop(s->dqlite);
	munit_assert_int(rv, ==, 0);

	dqlite_node_destroy(s->dqlite);

	testDirTearDown(s->dir);
}

void testServerStart(struct testServer *s)
{
	int client;
	int rv;

	rv = dqlite_node_create(s->id, s->address, s->dir, &s->dqlite);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(s->dqlite, s->address);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_connect_func(s->dqlite, endpointConnect, s);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_start(s->dqlite);
	munit_assert_int(rv, ==, 0);

	/* Connect a client. */
	rv = endpointConnect(NULL, s->address, &client);
	munit_assert_int(rv, ==, 0);

	rv = clientInit(&s->client, client);
	munit_assert_int(rv, ==, 0);
}

struct client *testServerClient(struct testServer *s)
{
	return &s->client;
}

static void setOther(struct testServer *s, struct testServer *other)
{
	unsigned i = other->id - 1;
	munit_assert_ptr_null(s->others[i]);
	s->others[i] = other;
}
void testServerNetwork(struct testServer *servers, unsigned nServers)
{
	unsigned i;
	unsigned j;
	for (i = 0; i < nServers; i++) {
		for (j = 0; j < nServers; j++) {
			struct testServer *server = &servers[i];
			struct testServer *other = &servers[j];
			if (i == j) {
				continue;
			}
			setOther(server, other);
		}
	}
}
