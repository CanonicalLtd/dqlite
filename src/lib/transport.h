/**
 * Asynchronously read and write buffer from and to the network.
 */

#ifndef LIB_TRANSPORT_H_
#define LIB_TRANSPORT_H_

#include <uv.h>

#define TRANSPORT_BADSOCKET 1000

/**
 * Callbacks.
 */
struct transport;
typedef void (*transportReadCb)(struct transport *t, int status);
typedef void (*transportWriteCb)(struct transport *t, int status);
typedef void (*transportCloseCb)(struct transport *t);

/**
 * Light wrapper around a libuv stream handle, providing a more convenient way
 * to read a certain amount of bytes.
 */
struct transport
{
	void *data;		     /* User defined */
	struct uv_stream_s *stream;  /* Data stream */
	uv_buf_t read;		     /* Read buffer */
	uv_write_t write;	    /* Write request */
	transportReadCb readCb;     /* Read callback */
	transportWriteCb writeCb;   /* Write callback */
	transportCloseCb closeCb;   /* Close callback */
};

/**
 * Initialize a transport of the appropriate type (TCP or PIPE) attached to the
 * given file descriptor.
 */
int transportInit(struct transport *t, struct uv_stream_s *stream);

/**
 * Start closing by the transport.
 */
void transportClose(struct transport *t, transportCloseCb cb);

/**
 * Read from the transport file descriptor until the given buffer is full.
 */
int transportRead(struct transport *t, uv_buf_t *buf, transportReadCb cb);

/**
 * Write the given buffer to the transport.
 */
int transportWrite(struct transport *t, uv_buf_t *buf, transportWriteCb cb);

/* Create an UV stream object from the given fd. */
int transportStream(struct uv_loop_s *loop,
		    int fd,
		    struct uv_stream_s **stream);

#endif /* LIB_TRANSPORT_H_ */
