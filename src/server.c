#include "server.h"

#include <stdlib.h>
#include <sys/un.h>
#include <time.h>

#include "../include/dqlite.h"
#include "conn.h"
#include "fsm.h"
#include "lib/assert.h"
#include "logger.h"
#include "transport.h"
#include "vfs.h"

/* Special ID for the bootstrap node. Equals to raft_digest("1", 0). */
#define BOOTSTRAP_ID 0x2dc171858c3155be

int dqliteInit(struct dqlite_node *d,
	       dqlite_node_id id,
	       const char *address,
	       const char *dir)
{
	int rv;
	memset(d->errmsg, 0, sizeof d->errmsg);
	rv = configInit(&d->config, id, address);
	if (rv != 0) {
		goto err;
	}
	rv = VfsInit(&d->vfs, d->config.name);
	sqlite3_vfs_register(&d->vfs, 0);
	if (rv != 0) {
		goto errAfterConfigInit;
	}
	registryInit(&d->registry, &d->config);
	rv = uv_loop_init(&d->loop);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto errAfterVfsInit;
	}
	rv = raftProxyInit(&d->raftTransport, &d->loop);
	if (rv != 0) {
		goto errAfterLoopInit;
	}
	rv = raft_uv_init(&d->raftIo, &d->loop, dir, &d->raftTransport);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto errAfterRaftTransportInit;
	}
	rv = fsmInit(&d->raftFsm, &d->config, &d->registry);
	if (rv != 0) {
		goto errAfterRaftIoInit;
	}

	/* TODO: properly handle closing the dqlite server without running it */
	rv = raft_init(&d->raft, &d->raftIo, &d->raftFsm, d->config.id,
		       d->config.address);
	if (rv != 0) {
		snprintf(d->errmsg, RAFT_ERRMSG_BUF_SIZE, "raft_init(): %s",
			 raft_errmsg(&d->raft));
		return rv;
	}
	/* TODO: expose these values through some API */
	raft_set_election_timeout(&d->raft, 3000);
	raft_set_heartbeat_timeout(&d->raft, 500);
	raft_set_snapshot_threshold(&d->raft, 1024);
	raft_set_snapshot_trailing(&d->raft, 8192);
	raft_set_pre_vote(&d->raft, true);
	raft_set_max_catch_up_rounds(&d->raft, 100);
	raft_set_max_catch_up_round_duration(&d->raft, 50 * 1000); /* 50 secs */
	rv = sem_init(&d->ready, 0, 0);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto errAfterRaftFsmInit;
	}
	rv = sem_init(&d->stopped, 0, 0);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto errAfterReadyInit;
	}

	rv = pthread_mutex_init(&d->mutex, NULL);
	assert(rv == 0); /* Docs say that pthread_mutex_init can't fail */
	QUEUE_INIT(&d->queue);
	QUEUE_INIT(&d->conns);
	d->raftState = RAFT_UNAVAILABLE;
	d->running = false;
	d->listener = NULL;
	d->bindAddress = NULL;
	return 0;

errAfterReadyInit:
	sem_destroy(&d->ready);
errAfterRaftFsmInit:
	fsmClose(&d->raftFsm);
errAfterRaftIoInit:
	raft_uv_close(&d->raftIo);
errAfterRaftTransportInit:
	raftProxyClose(&d->raftTransport);
errAfterLoopInit:
	uv_loop_close(&d->loop);
errAfterVfsInit:
	VfsClose(&d->vfs);
errAfterConfigInit:
	configClose(&d->config);
err:
	return rv;
}

void dqliteClose(struct dqlite_node *d)
{
	int rv;
	raft_free(d->listener);
	rv = pthread_mutex_destroy(&d->mutex); /* This is a no-op on Linux . */
	assert(rv == 0);
	rv = sem_destroy(&d->stopped);
	assert(rv == 0); /* Fails only if sem object is not valid */
	rv = sem_destroy(&d->ready);
	assert(rv == 0); /* Fails only if sem object is not valid */
	fsmClose(&d->raftFsm);
	uv_loop_close(&d->loop);
	raftProxyClose(&d->raftTransport);
	registryClose(&d->registry);
	sqlite3_vfs_unregister(&d->vfs);
	VfsClose(&d->vfs);
	configClose(&d->config);
	if (d->bindAddress != NULL) {
		sqlite3_free(d->bindAddress);
	}
}

int dqlite_node_create(dqlite_node_id id,
		       const char *address,
		       const char *dataDir,
		       dqlite_node **t)
{
	int rv;

	*t = sqlite3_malloc(sizeof **t);
	if (*t == NULL) {
		return DQLITE_NOMEM;
	}

	rv = dqliteInit(*t, id, address, dataDir);
	if (rv != 0) {
		sqlite3_free(*t);
		*t = NULL;
		return rv;
	}

	return 0;
}

static int ipParse(const char *address, struct sockaddr_in *addr)
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

int dqlite_node_set_bind_address(dqlite_node *t, const char *address)
{
	struct sockaddr_un addrUn;
	struct sockaddr_in addrIn;
	struct sockaddr *addr;
	size_t len;
	int fd;
	int rv;
	int domain = address[0] == '@' ? AF_UNIX : AF_INET;
	if (t->running) {
		return DQLITE_MISUSE;
	}
	if (domain == AF_INET) {
		memset(&addrIn, 0, sizeof addrIn);
		rv = ipParse(address, &addrIn);
		if (rv != 0) {
			return DQLITE_MISUSE;
		}
		len = sizeof addrIn;
		addr = (struct sockaddr *)&addrIn;
	} else {
		memset(&addrUn, 0, sizeof addrUn);
		addrUn.sun_family = AF_UNIX;
		len = strlen(address);
		if (len == 1) {
			/* Auto bind */
			len = 0;
		} else {
			strcpy(addrUn.sun_path + 1, address + 1);
		}
		len += sizeof(sa_family_t);
		addr = (struct sockaddr *)&addrUn;
	}
	fd = socket(domain, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd == -1) {
		return DQLITE_ERROR;
	}
	if (domain == AF_INET) {
		int reuse = 1;
		rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
				(const char *)&reuse, sizeof(reuse));
		if (rv != 0) {
			close(fd);
			return DQLITE_ERROR;
		}
	}

	rv = bind(fd, addr, (socklen_t)len);
	if (rv != 0) {
		close(fd);
		return DQLITE_ERROR;
	}

	rv = transportStream(&t->loop, fd, &t->listener);
	if (rv != 0) {
		close(fd);
		return DQLITE_ERROR;
	}

	if (domain == AF_INET) {
		t->bindAddress = sqlite3_malloc((int)strlen(address));
		if (t->bindAddress == NULL) {
			close(fd);
			return DQLITE_NOMEM;
		}
		strcpy(t->bindAddress, address);
	} else {
		len = sizeof addrUn.sun_path;
		t->bindAddress = sqlite3_malloc((int)len);
		if (t->bindAddress == NULL) {
			close(fd);
			return DQLITE_NOMEM;
		}
		memset(t->bindAddress, 0, len);
		rv = uv_pipe_getsockname((struct uv_pipe_s *)t->listener,
					 t->bindAddress, &len);
		if (rv != 0) {
			close(fd);
			sqlite3_free(t->bindAddress);
			t->bindAddress = NULL;
			return DQLITE_ERROR;
		}
		t->bindAddress[0] = '@';
	}

	return 0;
}

const char *dqliteNodeGetBindAddress(dqlite_node *t)
{
	return t->bindAddress;
}

int dqlite_node_set_connect_func(dqlite_node *t,
				 int (*f)(void *arg,
					  const char *address,
					  int *fd),
				 void *arg)
{
	if (t->running) {
		return DQLITE_MISUSE;
	}
	raftProxySetConnectFunc(&t->raftTransport, f, arg);
	return 0;
}

int dqliteNodeSetNetworkLatency(dqlite_node *t, unsigned long long nanoseconds)
{
	unsigned milliseconds;
	if (t->running) {
		return DQLITE_MISUSE;
	}
	/* Currently we accept at most 500 microseconds latency. */
	if (nanoseconds < 500 * 1000) {
		return DQLITE_MISUSE;
	}
	milliseconds = (unsigned)(nanoseconds / (1000 * 1000));
	raft_set_heartbeat_timeout(&t->raft, (milliseconds * 15) / 10);
	raft_set_election_timeout(&t->raft, milliseconds * 15);
	return 0;
}

int dqliteNodeSetFailureDomain(dqlite_node *n, unsigned long long code)
{
	n->config.failureDomain = code;
	return 0;
}

static int maybeBootstrap(dqlite_node *d,
			  dqlite_node_id id,
			  const char *address)
{
	struct raft_configuration configuration;
	int rv;
	if (id != 1 && id != BOOTSTRAP_ID) {
		return 0;
	}
	raft_configuration_init(&configuration);
	rv = raft_configuration_add(&configuration, id, address, true);
	if (rv != 0) {
		assert(rv == RAFT_NOMEM);
		rv = DQLITE_NOMEM;
		goto out;
	};
	rv = raft_bootstrap(&d->raft, &configuration);
	if (rv != 0) {
		if (rv == RAFT_CANTBOOTSTRAP) {
			rv = 0;
		} else {
			rv = DQLITE_ERROR;
		}
		goto out;
	}
out:
	raft_configuration_close(&configuration);
	return rv;
}

/* Callback invoked when the stop async handle gets fired.
 *
 * This callback will walk through all active handles and close them. After the
 * last handle (which must be the 'stop' async handle) is closed, the loop gets
 * stopped.
 */
static void raftCloseCb(struct raft *raft)
{
	struct dqlite_node *s = raft->data;
	raft_uv_close(&s->raftIo);
	uv_close((struct uv_handle_s *)&s->stop, NULL);
	uv_close((struct uv_handle_s *)&s->startup, NULL);
	uv_close((struct uv_handle_s *)&s->monitor, NULL);
	uv_close((struct uv_handle_s *)s->listener, NULL);
}

static void destroyConn(struct conn *conn)
{
	QUEUE_REMOVE(&conn->queue);
	sqlite3_free(conn);
}

static void stopCb(uv_async_t *stop)
{
	struct dqlite_node *d = stop->data;
	queue *head;
	struct conn *conn;

	/* We expect that we're being executed after dqliteStop and so the
	 * running flag is off. */
	assert(!d->running);

	QUEUE_FOREACH(head, &d->conns)
	{
		conn = QUEUE_DATA(head, struct conn, queue);
		connStop(conn);
	}
	raft_close(&d->raft, raftCloseCb);
}

/* Callback invoked as soon as the loop as started.
 *
 * It unblocks the s->ready semaphore.
 */
static void startupCb(uv_timer_t *startup)
{
	struct dqlite_node *d = startup->data;
	int rv;
	d->running = true;
	rv = sem_post(&d->ready);
	assert(rv == 0); /* No reason for which posting should fail */
}

static void listenCb(uv_stream_t *listener, int status)
{
	struct dqlite_node *t = listener->data;
	struct uv_stream_s *stream;
	struct conn *conn;
	int rv;

	if (status != 0) {
		/* TODO: log the error. */
		return;
	}

	switch (listener->type) {
		case UV_TCP:
			stream = raft_malloc(sizeof(struct uv_tcp_s));
			if (stream == NULL) {
				return;
			}
			rv = uv_tcp_init(&t->loop, (struct uv_tcp_s *)stream);
			assert(rv == 0);
			break;
		case UV_NAMED_PIPE:
			stream = raft_malloc(sizeof(struct uv_pipe_s));
			if (stream == NULL) {
				return;
			}
			rv = uv_pipe_init(&t->loop, (struct uv_pipe_s *)stream,
					  0);
			assert(rv == 0);
			break;
		default:
			assert(0);
	}

	rv = uv_accept(listener, stream);
	if (rv != 0) {
		goto err;
	}

	/* We accept unix socket connections only from the same process. */
	if (listener->type == UV_NAMED_PIPE) {
		struct ucred cred;
		socklen_t len;
		int fd;
		fd = stream->io_watcher.fd;
		len = sizeof cred;
		rv = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
		if (rv != 0) {
			goto err;
		}
		if (cred.pid != getpid()) {
			goto err;
		}
	}

	conn = sqlite3_malloc(sizeof *conn);
	if (conn == NULL) {
		goto err;
	}
	rv = connStart(conn, &t->config, &t->loop, &t->registry, &t->raft,
		       stream, &t->raftTransport, destroyConn);
	if (rv != 0) {
		goto errAfterConnAlloc;
	}

	QUEUE_PUSH(&t->conns, &conn->queue);

	return;

errAfterConnAlloc:
	sqlite3_free(conn);
err:
	uv_close((struct uv_handle_s *)stream, (uv_close_cb)raft_free);
}

static void monitorCb(uv_prepare_t *monitor)
{
	struct dqlite_node *d = monitor->data;
	int state = raft_state(&d->raft);
	/*
	queue *head;
	struct conn *conn;
	*/

	if (state == RAFT_UNAVAILABLE) {
		return;
	}

	/* TODO: we should shutdown clients that are performing SQL requests,
	 * but not the ones which are doing management-requests, such as
	 * transfer leadership.  */
	/*
	if (d->raftState == RAFT_LEADER && state != RAFT_LEADER) {
		QUEUE_FOREACH(head, &d->conns)
		{
			conn = QUEUE_DATA(head, struct conn, queue);
			connStop(conn);
		}
	}
	*/

	d->raftState = state;
}

static int taskRun(struct dqlite_node *d)
{
	int rv;

	/* TODO: implement proper cleanup upon error by spinning the loop a few
	 * times. */
	assert(d->listener != NULL);

	rv = uv_listen(d->listener, 128, listenCb);
	if (rv != 0) {
		return rv;
	}
	d->listener->data = d;

	/* Initialize notification handles. */
	d->stop.data = d;
	rv = uv_async_init(&d->loop, &d->stop, stopCb);
	assert(rv == 0);

	/* Schedule startupCb to be fired as soon as the loop starts. It will
	 * unblock clients of taskReady. */
	d->startup.data = d;
	rv = uv_timer_init(&d->loop, &d->startup);
	assert(rv == 0);
	rv = uv_timer_start(&d->startup, startupCb, 0, 0);
	assert(rv == 0);

	/* Schedule raft state change monitor. */
	d->monitor.data = d;
	rv = uv_prepare_init(&d->loop, &d->monitor);
	assert(rv == 0);
	rv = uv_prepare_start(&d->monitor, monitorCb);
	assert(rv == 0);

	d->raft.data = d;
	rv = raft_start(&d->raft);
	if (rv != 0) {
		snprintf(d->errmsg, RAFT_ERRMSG_BUF_SIZE, "raft_start(): %s",
			 raft_errmsg(&d->raft));
		/* Unblock any client of taskReady */
		sem_post(&d->ready);
		return rv;
	}

	rv = uv_run(&d->loop, UV_RUN_DEFAULT);
	assert(rv == 0);

	/* Unblock any client of taskReady */
	rv = sem_post(&d->ready);
	assert(rv == 0); /* no reason for which posting should fail */

	return 0;
}

const char *dqliteNodeErrmsg(dqlite_node *n)
{
	return n->errmsg;
}

static void *taskStart(void *arg)
{
	struct dqlite_node *t = arg;
	int rv;
	rv = taskRun(t);
	if (rv != 0) {
		uintptr_t result = (uintptr_t)rv;
		return (void *)result;
	}
	return NULL;
}

void dqlite_node_destroy(dqlite_node *d)
{
	dqliteClose(d);
	sqlite3_free(d);
}

/* Wait until a dqlite server is ready and can handle connections.
**
** Returns true if the server has been successfully started, false otherwise.
**
** This is a thread-safe API, but must be invoked before any call to
** dqliteStop or dqliteHandle.
*/
static bool taskReady(struct dqlite_node *d)
{
	/* Wait for the ready semaphore */
	sem_wait(&d->ready);
	return d->running;
}

int dqlite_node_start(dqlite_node *t)
{
	int rv;

	rv = maybeBootstrap(t, t->config.id, t->config.address);
	if (rv != 0) {
		goto err;
	}

	rv = pthread_create(&t->thread, 0, &taskStart, t);
	if (rv != 0) {
		goto err;
	}

	if (!taskReady(t)) {
		rv = DQLITE_ERROR;
		goto err;
	}

	return 0;

err:
	return rv;
}

int dqlite_node_stop(dqlite_node *d)
{
	void *result;
	int rv;

	/* Grab the queue mutex, so we can be sure no new incoming request will
	 * be enqueued from this point on. */
	pthread_mutex_lock(&d->mutex);

	/* Turn off the running flag, so calls to dqliteHandle will fail
	 * with DQLITE_STOPPED. This needs to happen before we send the stop
	 * signal since the stop callback expects to see that the flag is
	 * off. */
	d->running = false;

	rv = uv_async_send(&d->stop);
	assert(rv == 0);

	pthread_mutex_unlock(&d->mutex);

	rv = pthread_join(d->thread, &result);
	assert(rv == 0);

	return (int)((uintptr_t)result);
}

int dqliteNodeRecover(dqlite_node *n,
		      struct dqlite_node_info infos[],
		      int nInfo)
{
	struct raft_configuration configuration;
	int i;
	int rv;

	raft_configuration_init(&configuration);
	for (i = 0; i < nInfo; i++) {
		struct dqlite_node_info *info = &infos[i];
		rv = raft_configuration_add(&configuration, info->id,
					    info->address, true);
		if (rv != 0) {
			assert(rv == RAFT_NOMEM);
			rv = DQLITE_NOMEM;
			goto out;
		};
	}

	rv = raft_recover(&n->raft, &configuration);
	if (rv != 0) {
		rv = DQLITE_ERROR;
		goto out;
	}

out:
	raft_configuration_close(&configuration);
	return rv;
}

dqlite_node_id dqliteGenerateNodeId(const char *address)
{
	struct timespec ts;
	int rv;
	unsigned long long n;

	rv = clock_gettime(CLOCK_REALTIME, &ts);
	assert(rv == 0);

	n = (unsigned long long)(ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec);

	return raft_digest(address, n);
}
