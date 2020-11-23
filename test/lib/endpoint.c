#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "endpoint.h"

static int getFamily(const MunitParameter params[])
{
	const char *family = NULL;
	if (params != NULL) {
		family = munit_parameters_get(params, TEST_ENDPOINT_FAMILY);
	}
	if (family == NULL) {
		family = "unix";
	}
	if (strcmp(family, "tcp") == 0) {
		return AF_INET;
	} else if (strcmp(family, "unix") == 0) {
		return AF_UNIX;
	}
	munit_errorf("unexpected socket family: %s", family);
	return -1;
}

void testEndpoint_setup(struct testEndpoint *e, const MunitParameter params[])
{
	struct sockaddr *address;
	socklen_t size;
	int rv;
	e->family = getFamily(params);

	/* Initialize the appropriate socket address structure, depending on the
	 * selected socket family. */
	switch (e->family) {
		case AF_INET:
			/* TCP socket on loopback device */
			memset(&e->inAddress, 0, sizeof e->inAddress);
			e->inAddress.sin_family = AF_INET;
			e->inAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
			e->inAddress.sin_port = 0; /* Get a random free port */
			address = (struct sockaddr *)(&e->inAddress);
			size = sizeof e->inAddress;
			break;
		case AF_UNIX:
			/* Abstract Unix socket */
			memset(&e->un_address, 0, sizeof e->un_address);
			e->un_address.sun_family = AF_UNIX;
			strcpy(e->un_address.sun_path, ""); /* Random address */
			address = (struct sockaddr *)(&e->un_address);
			size = sizeof e->un_address;
			break;
		default:
			munit_errorf("unexpected socket family: %d", e->family);
	}

	/* Create the listener fd. */
	e->fd = socket(e->family, SOCK_STREAM, 0);
	if (e->fd < 0) {
		munit_errorf("socket(): %s", strerror(errno));
	}

	/* Bind the listener fd. */
	rv = bind(e->fd, address, size);
	if (rv != 0) {
		munit_errorf("bind(): %s", strerror(errno));
	}

	/* Get the actual addressed assigned by the kernel and save it back in
	 * the relevant struct server field (pointed to by address). */
	rv = getsockname(e->fd, address, &size);
	if (rv != 0) {
		munit_errorf("getsockname(): %s", strerror(errno));
	}

	/* Render the endpoint address. */
	switch (e->family) {
		case AF_INET:
			sprintf(e->address, "127.0.0.1:%d",
				htons(e->inAddress.sin_port));
			break;
		case AF_UNIX:
			/* TODO */
			break;
	}
}

void testEndpoint_tear_down(struct testEndpoint *e)
{
	close(e->fd);
}

int testEndpoint_connect(struct testEndpoint *e)
{
	struct sockaddr *address;
	socklen_t size;
	int fd;
	int rv;

	switch (e->family) {
		case AF_INET:
			address = (struct sockaddr *)&e->inAddress;
			size = sizeof e->inAddress;
			break;
		case AF_UNIX:
			address = (struct sockaddr *)&e->un_address;
			size = sizeof e->un_address;
			break;
	}

	/* Create the socket. */
	fd = socket(e->family, SOCK_STREAM, 0);
	if (fd < 0) {
		munit_errorf("socket(): %s", strerror(errno));
	}

	/* Connect to the server */
	rv = connect(fd, address, size);
	if (rv != 0 && errno != ECONNREFUSED) {
		munit_errorf("connect(): %s", strerror(errno));
	}

	return fd;
}

int testEndpoint_accept(struct testEndpoint *e)
{
	struct sockaddr_in inAddress;
	struct sockaddr_un un_address;
	struct sockaddr *address;
	socklen_t size;
	int fd;
	int rv;

	switch (e->family) {
		case AF_INET:
			address = (struct sockaddr *)&inAddress;
			size = sizeof inAddress;
			break;
		case AF_UNIX:
			address = (struct sockaddr *)&un_address;
			size = sizeof un_address;
			break;
	}

	/* Accept the client connection. */
	fd = accept(e->fd, address, &size);
	if (fd < 0) {
		/* Check if the endpoint has been closed, so this is benign. */
		if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
			return -1;
		}
		munit_errorf("accept(): %s", strerror(errno));
	}

	/* Set non-blocking mode */
	rv = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (rv != 0) {
		munit_errorf("set non-blocking mode: %s", strerror(errno));
	}

	return fd;
}

void testEndpoint_pair(struct testEndpoint *e, int *server, int *client)
{
	*client = testEndpoint_connect(e);
	*server = testEndpoint_accept(e);
}

const char *testEndpoint_address(struct testEndpoint *e)
{
	return e->address;
}

char *testEndpoint_family_values[] = {"tcp", "unix", NULL};
