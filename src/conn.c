#include "conn.h"
#include "message.h"
#include "request.h"
#include "transport.h"
#include "protocol.h"

/* Initialize the given buffer for reading, ensure it has the given size. */
static int initRead(struct conn *c, uv_buf_t *buf, size_t size)
{
	bufferReset(&c->read);
	buf->base = bufferAdvance(&c->read, size);
	if (buf->base == NULL) {
		return DQLITE_NOMEM;
	}
	buf->len = size;
	return 0;
}

static int readMessage(struct conn *c);
static void writeCb(struct transport *transport, int status)
{
	struct conn *c = transport->data;
	bool finished;
	int rv;
	if (status != 0) {
		goto abort;
	}

	bufferReset(&c->write);
	bufferAdvance(&c->write, messageSizeof(&c->response)); /* Header */

	rv = gatewayResume(&c->gateway, &finished);
	if (rv != 0) {
		goto abort;
	}
	if (!finished) {
		return;
	}

	/* Start reading the next request */
	rv = readMessage(c);
	if (rv != 0) {
		goto abort;
	}
	return;
abort:
	connStop(c);
}

static void gatewayHandleCb(struct handle *req, int status, int type)
{
	struct conn *c = req->data;
	size_t n;
	void *cursor;
	uv_buf_t buf;
	int rv;

	/* Ignore results firing after we started closing. TODO: instead, we
	 * should make gatewayClose() asynchronous. */
	if (c->closed) {
		return;
	}

	if (status != 0) {
		goto abort;
	}

	n = bufferOffset(&c->write) - messageSizeof(&c->response);
	assert(n % 8 == 0);

	c->response.type = (uint8_t)type;
	c->response.words = (uint32_t)(n / 8);
	c->response.flags = 0;
	c->response.extra = 0;

	cursor = bufferCursor(&c->write, 0);
	messageEncode(&c->response, &cursor);

	buf.base = bufferCursor(&c->write, 0);
	buf.len = bufferOffset(&c->write);

	rv = transportWrite(&c->transport, &buf, writeCb);
	if (rv != 0) {
		goto abort;
	}
	return;
abort:
	connStop(c);
}

static void closeCb(struct transport *transport)
{
	struct conn *c = transport->data;
	bufferClose(&c->write);
	bufferClose(&c->read);
	if (c->closeCb != NULL) {
		c->closeCb(c);
	}
}

static void raftConnect(struct conn *c, struct cursor *cursor)
{
	struct requestConnect request;
	int rv;
	rv = requestConnectDecode(cursor, &request);
	if (rv != 0) {
		connStop(c);
		return;
	}
	raftProxyAccept(c->uvTransport, request.id, request.address,
			c->transport.stream);
	/* Close the connection without actually closing the transport, since
	 * the stream will be used by raft */
	c->closed = true;
	closeCb(&c->transport);
}

static void readRequestCb(struct transport *transport, int status)
{
	struct conn *c = transport->data;
	struct cursor cursor;
	int rv;

	if (status != 0) {
		// errorf(c->logger, "read error");
		connStop(c);
		return;
	}

	cursor.p = bufferCursor(&c->read, 0);
	cursor.cap = bufferOffset(&c->read);

	bufferReset(&c->write);
	bufferAdvance(&c->write, messageSizeof(&c->response)); /* Header */

	switch (c->request.type) {
		case DQLITE_REQUEST_CONNECT:
			raftConnect(c, &cursor);
			return;
	}

	rv = gatewayHandle(&c->gateway, &c->handle, c->request.type, &cursor,
			   &c->write, gatewayHandleCb);
	if (rv != 0) {
		connStop(c);
	}
}

/* Start reading the body of the next request */
static int readRequest(struct conn *c)
{
	uv_buf_t buf;
	int rv;
	rv = initRead(c, &buf, c->request.words * 8);
	if (rv != 0) {
		return rv;
	}
	rv = transportRead(&c->transport, &buf, readRequestCb);
	if (rv != 0) {
		return rv;
	}
	return 0;
}

static void readMessageCb(struct transport *transport, int status)
{
	struct conn *c = transport->data;
	struct cursor cursor;
	int rv;

	if (status != 0) {
		// errorf(c->logger, "read error");
		connStop(c);
		return;
	}

	cursor.p = bufferCursor(&c->read, 0);
	cursor.cap = bufferOffset(&c->read);

	rv = messageDecode(&cursor, &c->request);
	assert(rv == 0); /* Can't fail, we know we have enough bytes */

	rv = readRequest(c);
	if (rv != 0) {
		connStop(c);
		return;
	}
}

/* Start reading metadata about the next message */
static int readMessage(struct conn *c)
{
	uv_buf_t buf;
	int rv;
	rv = initRead(c, &buf, messageSizeof(&c->request));
	if (rv != 0) {
		return rv;
	}
	rv = transportRead(&c->transport, &buf, readMessageCb);
	if (rv != 0) {
		return rv;
	}
	return 0;
}

static void readProtocolCb(struct transport *transport, int status)
{
	struct conn *c = transport->data;
	struct cursor cursor;
	int rv;

	if (status != 0) {
		// errorf(c->logger, "read error");
		goto abort;
	}

	cursor.p = bufferCursor(&c->read, 0);
	cursor.cap = bufferOffset(&c->read);

	rv = uint64Decode(&cursor, &c->protocol);
	assert(rv == 0); /* Can't fail, we know we have enough bytes */

	if (c->protocol != DQLITE_PROTOCOL_VERSION && c->protocol != DQLITE_PROTOCOL_VERSION_LEGACY) {
		/* errorf(c->logger, "unknown protocol version: %lx", */
		/* c->protocol); */
		/* TODO: instead of closing the connection we should return
		 * error messages */
		goto abort;
	}
	c->gateway.protocol = c->protocol;

	rv = readMessage(c);
	if (rv != 0) {
		goto abort;
	}

	return;
abort:
	connStop(c);
}

/* Start reading the protocol format version */
static int readProtocol(struct conn *c)
{
	uv_buf_t buf;
	int rv;
	rv = initRead(c, &buf, sizeof c->protocol);
	if (rv != 0) {
		return rv;
	}
	rv = transportRead(&c->transport, &buf, readProtocolCb);
	if (rv != 0) {
		return rv;
	}
	return 0;
}

int connStart(struct conn *c,
	      struct config *config,
	      struct uv_loop_s *loop,
	      struct registry *registry,
	      struct raft *raft,
	      struct uv_stream_s *stream,
	      struct raft_uv_transport *uvTransport,
	      conn_closeCb close_cb)
{
	int rv;
	(void)loop;
	rv = transportInit(&c->transport, stream);
	if (rv != 0) {
		goto err;
	}
	c->config = config;
	c->transport.data = c;
	c->uvTransport = uvTransport;
	c->closeCb = close_cb;
	gatewayInit(&c->gateway, config, registry, raft);
	rv = bufferInit(&c->read);
	if (rv != 0) {
		goto errAfterTransportInit;
	}
	rv = bufferInit(&c->write);
	if (rv != 0) {
		goto errAfterReadBufferInit;
	}
	c->handle.data = c;
	c->closed = false;
	/* First, we expect the client to send us the protocol version. */
	rv = readProtocol(c);
	if (rv != 0) {
		goto errAfterWriteBufferInit;
	}
	return 0;

errAfterWriteBufferInit:
	bufferClose(&c->write);
errAfterReadBufferInit:
	bufferClose(&c->read);
errAfterTransportInit:
	transportClose(&c->transport, NULL);
err:
	return rv;
}

void connStop(struct conn *c)
{
	if (c->closed) {
		return;
	}
	c->closed = true;
	gatewayClose(&c->gateway);
	transportClose(&c->transport, closeCb);
}
