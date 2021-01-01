#include "lib/transport.h"

#include <raft.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "message.h"
#include "request.h"
#include "transport.h"

struct impl
{
	struct uv_loop_s *loop;
	struct
	{
		int (*f)(void *arg, const char *address, int *fd);
		void *arg;

	} connect;
	raft_id id;
	const char *address;
	raft_uv_accept_cb acceptCb;
};

struct connect
{
	struct impl *impl;
	struct raft_uv_connect *req;
	struct uv_work_s work;
	raft_id id;
	const char *address;
	int fd;
	int status;
};

static int implInit(struct raft_uv_transport *transport,
		    raft_id id,
		    const char *address)
{
	struct impl *i = transport->impl;
	i->id = id;
	i->address = address;
	return 0;
}

static int implListen(struct raft_uv_transport *transport, raft_uv_accept_cb cb)
{
	struct impl *i = transport->impl;
	i->acceptCb = cb;
	return 0;
}

static void connectWorkCb(uv_work_t *work)
{
	struct connect *r = work->data;
	struct impl *i = r->impl;
	struct message message;
	struct requestConnect request;
	uint64_t protocol;
	void *buf;
	void *cursor;
	size_t n;
	size_t n1;
	size_t n2;
	int rv;

	/* Establish a connection to the other node using the provided connect
	 * function. */
	rv = i->connect.f(i->connect.arg, r->address, &r->fd);
	if (rv != 0) {
		rv = RAFT_NOCONNECTION;
		goto err;
	}

	/* Send the initial dqlite protocol handshake. */
	protocol = byteFlip64(DQLITE_PROTOCOL_VERSION);
	rv = (int)write(r->fd, &protocol, sizeof protocol);
	if (rv != sizeof protocol) {
		rv = RAFT_NOCONNECTION;
		goto errAfterConnect;
	}

	/* Send a CONNECT dqlite protocol command, which will transfer control
	 * to the underlying raft UV backend. */
	request.id = i->id;
	request.address = i->address;

	n1 = messageSizeof(&message);
	n2 = requestConnectSizeof(&request);

	message.type = DQLITE_REQUEST_CONNECT;
	message.words = (uint32_t)(n2 / 8);

	n = n1 + n2;

	buf = sqlite3_malloc64(n);
	if (buf == NULL) {
		rv = RAFT_NOCONNECTION;
		goto errAfterConnect;
	}

	cursor = buf;
	messageEncode(&message, &cursor);
	requestConnectEncode(&request, &cursor);

	rv = (int)write(r->fd, buf, n);
	sqlite3_free(buf);

	if (rv != (int)n) {
		rv = RAFT_NOCONNECTION;
		goto errAfterConnect;
	}

	r->status = 0;
	return;

errAfterConnect:
	close(r->fd);
err:
	r->status = rv;
	return;
}

static void connectAfterWorkCb(uv_work_t *work, int status)
{
	struct connect *r = work->data;
	struct impl *i = r->impl;
	struct uv_stream_s *stream = NULL;
	int rv;

	assert(status == 0);

	if (r->status != 0) {
		goto out;
	}

	rv = transportStream(i->loop, r->fd, &stream);
	if (rv != 0) {
		r->status = RAFT_NOCONNECTION;
		close(r->fd);
		goto out;
	}
out:
	r->req->cb(r->req, stream, r->status);
	sqlite3_free(r);
}

static int implConnect(struct raft_uv_transport *transport,
		       struct raft_uv_connect *req,
		       raft_id id,
		       const char *address,
		       raft_uv_connect_cb cb)
{
	struct impl *i = transport->impl;
	struct connect *r;
	int rv;

	r = sqlite3_malloc(sizeof *r);
	if (r == NULL) {
		rv = DQLITE_NOMEM;
		goto err;
	}

	r->impl = i;
	r->req = req;
	r->work.data = r;
	r->id = id;
	r->address = address;

	req->cb = cb;

	rv =
	    uv_queue_work(i->loop, &r->work, connectWorkCb, connectAfterWorkCb);
	if (rv != 0) {
		rv = RAFT_NOCONNECTION;
		goto errAfterConnectAlloc;
	}

	return 0;

errAfterConnectAlloc:
	sqlite3_free(r);
err:
	return rv;
}

static void implClose(struct raft_uv_transport *transport,
		      raft_uv_transport_close_cb cb)
{
	cb(transport);
}

static int parseAddress(const char *address, struct sockaddr_in *addr)
{
	char buf[256];
	char *host;
	char *port;
	char *colon = ":";
	int rv;

	/* TODO: turn this poor man parsing into proper one */
	strcpy(buf, address);
	host = strtok(buf, colon);
	port = strtok(NULL, ":");
	if (port == NULL) {
		port = "8080";
	}

	rv = uv_ip4_addr(host, atoi(port), addr);
	if (rv != 0) {
		return RAFT_NOCONNECTION;
	}

	return 0;
}

static int defaultConnect(void *arg, const char *address, int *fd)
{
	struct sockaddr_in addr;
	int rv;
	(void)arg;

	rv = parseAddress(address, &addr);
	if (rv != 0) {
		return RAFT_NOCONNECTION;
	}

	*fd = socket(AF_INET, SOCK_STREAM, 0);
	if (*fd == -1) {
		return RAFT_NOCONNECTION;
	}

	rv = connect(*fd, (const struct sockaddr *)&addr, sizeof addr);
	if (rv == -1) {
		close(*fd);
		return RAFT_NOCONNECTION;
	}

	return 0;
}

int raftProxyInit(struct raft_uv_transport *transport, struct uv_loop_s *loop)
{
	struct impl *i = sqlite3_malloc(sizeof *i);
	if (i == NULL) {
		return DQLITE_NOMEM;
	}
	i->loop = loop;
	i->connect.f = defaultConnect;
	i->connect.arg = NULL;
	i->acceptCb = NULL;
	transport->impl = i;
	transport->init = implInit;
	transport->listen = implListen;
	transport->connect = implConnect;
	transport->close = implClose;
	return 0;
}

void raftProxyClose(struct raft_uv_transport *transport)
{
	struct impl *i = transport->impl;
	sqlite3_free(i);
}

void raftProxyAccept(struct raft_uv_transport *transport,
		     raft_id id,
		     const char *address,
		     struct uv_stream_s *stream)
{
	struct impl *i = transport->impl;
	/* If the accept callback is NULL it means we were stopped. */
	if (i->acceptCb == NULL) {
		uv_close((struct uv_handle_s *)stream, (uv_close_cb)raft_free);
	} else {
		i->acceptCb(transport, id, address, stream);
	}
}

void raftProxySetConnectFunc(struct raft_uv_transport *transport,
			     int (*f)(void *arg, const char *address, int *fd),
			     void *arg)
{
	struct impl *i = transport->impl;
	i->connect.f = f;
	i->connect.arg = arg;
}
