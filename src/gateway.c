#include <float.h>
#include <stdio.h>

#ifdef DQLITE_EXPERIMENTAL

#include <libco.h>

#endif /* DQLITE_EXPERIMENTAL */

#include "../include/dqlite.h"

#include "assert.h"
#include "error.h"
#include "format.h"
#include "gateway.h"
#include "lifecycle.h"
#include "log.h"
#include "request.h"
#include "response.h"

/* Perform a distributed checkpoint if the size of the WAL has reached the
 * configured threshold and there are no reading transactions in progress (there
 * can't be writing transaction because this helper gets called after a
 * successful commit). */
static int maybe_checkpoint(void *ctx,
			    sqlite3 *db,
			    const char *schema,
			    int pages)
{
	struct gateway *g;
	struct sqlite3_file *file;
	volatile void *region;
	uint32_t mx_frame;
	uint32_t read_marks[FORMAT__WAL_NREADER];
	int rc;
	int i;

	(void)schema;

	assert(ctx != NULL);
	assert(db != NULL);

	g = ctx;

	/* Check if the size of the WAL is beyond the threshold. */
	if ((unsigned)pages < g->options->checkpoint_threshold) {
		/* Nothing to do yet. */
		return SQLITE_OK;
	}

	/* Get the database file associated with this connection */
	rc = sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rc == SQLITE_OK); /* Should never fail */

	/* Get the first SHM region, which contains the WAL header. */
	rc = file->pMethods->xShmMap(file, 0, 0, 0, &region);
	assert(rc == SQLITE_OK); /* Should never fail */

	/* Get the current value of mxFrame. */
	format__get_mx_frame((const uint8_t *)region, &mx_frame);

	/* Get the content of the read marks. */
	format__get_read_marks((const uint8_t *)region, read_marks);

	/* Check each mark and associated lock. This logic is similar to the one
	 * in the walCheckpoint function of wal.c, in the SQLite code. */
	for (i = 0; i < SQLITE_SHM_NLOCK; i++) {
		int flags = SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE;

		rc = file->pMethods->xShmLock(file, i, 1, flags);
		if (rc == SQLITE_BUSY) {
			/* It's locked. Let's postpone the checkpoint
			 * for now. */
			return SQLITE_OK;
		}

		/* Not locked. Let's release the lock we just
		 * acquired. */
		flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE;
		file->pMethods->xShmLock(file, i, 1, flags);
	}

	/* Attempt to perfom a checkpoint across all nodes.
	 *
	 * TODO: reason about if it's indeed fine to ignore all kind of
	 * errors. */
	g->cluster->xCheckpoint(g->cluster->ctx, db);

	return SQLITE_OK;
}

/* Release dynamically allocated data attached to a response after it has been
 * flushed. */
static void reset_response(struct response *r)
{
	int i;

	/* TODO: we use free() instead of sqlite3_free() below because Go's
	 * C.CString() will allocate strings using malloc. Once we switch to a
	 * pure C implementation, we can use sqlite3_free instead. */
	switch (r->type) {
		case DQLITE_RESPONSE_SERVER:
			assert(r->server.address != NULL);

			free((char *)r->server.address);
			r->server.address = NULL;

			break;

		case DQLITE_RESPONSE_SERVERS:
			assert(r->servers.servers != NULL);

			for (i = 0; r->servers.servers[i].address != NULL;
			     i++) {
				free((char *)r->servers.servers[i].address);
			}

			free(r->servers.servers);
			r->servers.servers = NULL;

			break;
	}
}

/* Render a failure response. */
static void gateway__failure(struct gateway *g,
			     struct gateway__ctx *ctx,
			     int code)
{
	ctx->response.type = DQLITE_RESPONSE_FAILURE;
	ctx->response.failure.code = code;
	ctx->response.failure.message = g->error;
}

static void gateway__leader(struct gateway *g, struct gateway__ctx *ctx)
{
	const char *address;

	address = g->cluster->xLeader(g->cluster->ctx);

	if (address == NULL) {
		dqlite__error_oom(&g->error, "failed to get cluster leader");
		gateway__failure(g, ctx, SQLITE_NOMEM);
		return;
	}
	ctx->response.type = DQLITE_RESPONSE_SERVER;
	ctx->response.server.address = address;
}

static void gateway__client(struct gateway *g, struct gateway__ctx *ctx)
{
	/* TODO: handle client registrations */

	ctx->response.type = DQLITE_RESPONSE_WELCOME;
	ctx->response.welcome.heartbeat_timeout = g->options->heartbeat_timeout;
}

static void gateway__heartbeat(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct dqlite_server_info *servers;

	/* Get the current list of servers in the cluster */
	rc = g->cluster->xServers(g->cluster->ctx, &servers);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error,
				     "failed to get cluster servers");
		gateway__failure(g, ctx, rc);
		return;
	}

	assert(servers != NULL);

	ctx->response.type = DQLITE_RESPONSE_SERVERS;
	ctx->response.servers.servers = servers;

	/* Refresh the heartbeat timestamp. */
	g->heartbeat = ctx->request->timestamp;
}

static void gateway__open(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;

	assert(g != NULL);

	if (g->db != NULL) {
		dqlite__error_printf(
		    &g->error,
		    "a database for this connection is already open");
		gateway__failure(g, ctx, SQLITE_BUSY);
		return;
	}

	g->db = sqlite3_malloc(sizeof *g->db);
	if (g->db == NULL) {
		dqlite__error_oom(&g->error, "unable to create database");
		gateway__failure(g, ctx, SQLITE_NOMEM);
		return;
	}

	db__init(g->db);

	g->db->id = 0;

	rc = db__open(g->db, ctx->request->open.name, ctx->request->open.flags,
		      g->options->vfs, g->options->page_size,
		      g->options->wal_replication);

	if (rc != 0) {
		dqlite__error_printf(&g->error, g->db->error);
		gateway__failure(g, ctx, rc);
		db__close(g->db);
		sqlite3_free(g->db);
		g->db = NULL;
		return;
	}

	sqlite3_wal_hook(g->db->db, maybe_checkpoint, g);

	ctx->response.type = DQLITE_RESPONSE_DB;
	ctx->response.db.id = (uint32_t)g->db->id;

	/* Notify the cluster implementation about the new connection. */
	g->cluster->xRegister(g->cluster->ctx, g->db->db);
	g->db->cluster = g->cluster;
}

/* Ensure that there are no raft logs pending. */
#define GATEWAY__BARRIER                                                \
	rc = g->cluster->xBarrier(g->cluster->ctx);                     \
	if (rc != 0) {                                                  \
		dqlite__error_printf(&g->error, "raft barrier failed"); \
		gateway__failure(g, ctx, rc);                           \
		return;                                                 \
	}

/* Lookup the database with the given ID. */
#define GATEWAY__LOOKUP_DB(ID)                                           \
	db = g->db;                                                      \
	if (db == NULL || db->id != ID) {                                \
		dqlite__error_printf(&g->error, "no db with id %d", ID); \
		gateway__failure(g, ctx, SQLITE_NOTFOUND);               \
		return;                                                  \
	}

/* Lookup the statement with the given ID. */
#define GATEWAY__LOOKUP_STMT(ID)                                           \
	stmt = db__stmt(db, ID);                                           \
	if (stmt == NULL) {                                                \
		dqlite__error_printf(&g->error, "no stmt with id %d", ID); \
		gateway__failure(g, ctx, SQLITE_NOTFOUND);                 \
		return;                                                    \
	}

static void gateway__prepare(struct gateway *g, struct gateway__ctx *ctx)
{
	struct db *db;
	struct stmt *stmt;
	int rc;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->prepare.db_id);

	rc = db__prepare(db, ctx->request->prepare.sql, &stmt);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error, db->error);
		gateway__failure(g, ctx, rc);
		return;
	}

	ctx->response.type = DQLITE_RESPONSE_STMT;
	ctx->response.stmt.db_id = ctx->request->prepare.db_id;
	ctx->response.stmt.id = stmt->id;
	ctx->response.stmt.params = sqlite3_bind_parameter_count(stmt->stmt);
}

static void gateway__exec(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct db *db;
	struct stmt *stmt;
	uint64_t last_insert_id;
	uint64_t rows_affected;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->exec.db_id);
	GATEWAY__LOOKUP_STMT(ctx->request->exec.stmt_id);

	assert(stmt != NULL);

	rc = stmt__bind(stmt, &ctx->request->message);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error, stmt->error);
		gateway__failure(g, ctx, rc);
		return;
	}

	rc = stmt__exec(stmt, &last_insert_id, &rows_affected);
	if (rc == SQLITE_OK) {
		ctx->response.type = DQLITE_RESPONSE_RESULT;
		ctx->response.result.last_insert_id = last_insert_id;
		ctx->response.result.rows_affected = rows_affected;
	} else {
		dqlite__error_printf(&g->error, stmt->error);
		gateway__failure(g, ctx, rc);
		sqlite3_reset(stmt->stmt);
	}
}

/* Step through the tiven statement and populate the response of the given
 * context with a single batch of rows.
 *
 * A single batch of rows is typically about the size of the static response
 * message body. */
static void gateway__query_batch(struct gateway *g,
				 struct db *db,
				 struct stmt *stmt,
				 struct gateway__ctx *ctx)
{
	int rc;

	rc = stmt__query(stmt, &ctx->response.message);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		sqlite3_reset(stmt->stmt);

		/* Finalize the statement if needed. */
		if (ctx->cleanup == GATEWAY__CLEANUP_FINALIZE) {
			db__finalize(db, stmt);
		}

		/* TODO: reset what was written in the message */
		dqlite__error_printf(&g->error, stmt->error);
		gateway__failure(g, ctx, rc);

		ctx->db = NULL;
		ctx->stmt = NULL;
		ctx->cleanup = GATEWAY__CLEANUP_NONE;
	} else {
		ctx->response.type = DQLITE_RESPONSE_ROWS;
		assert(rc == SQLITE_ROW || rc == SQLITE_DONE);
		if (rc == SQLITE_ROW) {
			ctx->response.rows.eof = DQLITE_RESPONSE_ROWS_PART;
			ctx->db = db;
			ctx->stmt = stmt;
		} else {
			/* Finalize the statement if needed. */
			if (ctx->cleanup == GATEWAY__CLEANUP_FINALIZE) {
				db__finalize(db, stmt);
			}

			/* Reset the multi-response info and the cleanup code */
			ctx->db = NULL;
			ctx->stmt = NULL;
			ctx->cleanup = GATEWAY__CLEANUP_NONE;

			ctx->response.rows.eof = DQLITE_RESPONSE_ROWS_DONE;
		}
	}
}

static void gateway__query(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct db *db;
	struct stmt *stmt;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->query.db_id);
	GATEWAY__LOOKUP_STMT(ctx->request->query.stmt_id);

	assert(stmt != NULL);

	rc = stmt__bind(stmt, &ctx->request->message);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error, stmt->error);
		gateway__failure(g, ctx, rc);
		return;
	}

	/* Set the cleanup code to none, since there's nothing to do
	 * once the request is done. */
	ctx->cleanup = GATEWAY__CLEANUP_NONE;

	gateway__query_batch(g, db, stmt, ctx);
}

static void gateway__finalize(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct db *db;
	struct stmt *stmt;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->finalize.db_id);
	GATEWAY__LOOKUP_STMT(ctx->request->finalize.stmt_id);

	rc = db__finalize(db, stmt);
	if (rc == SQLITE_OK) {
		ctx->response.type = DQLITE_RESPONSE_EMPTY;
	} else {
		dqlite__error_printf(&g->error, db->error);
		gateway__failure(g, ctx, rc);
	}
}

static void gateway__exec_sql(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct db *db;
	const char *sql;
	struct stmt *stmt = NULL;
	uint64_t last_insert_id;
	uint64_t rows_affected;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->exec_sql.db_id);

	assert(db != NULL);

	sql = ctx->request->exec_sql.sql;

	while (sql != NULL && strcmp(sql, "") != 0) {
		rc = db__prepare(db, sql, &stmt);
		if (rc != SQLITE_OK) {
			dqlite__error_printf(&g->error, db->error);
			gateway__failure(g, ctx, rc);
			return;
		}

		if (stmt->stmt == NULL) {
			return;
		}

		/* TODO: what about bindings for multi-statement SQL text? */
		rc = stmt__bind(stmt, &ctx->request->message);
		if (rc != SQLITE_OK) {
			dqlite__error_printf(&g->error, stmt->error);
			gateway__failure(g, ctx, rc);
			goto err;
			return;
		}

		rc = stmt__exec(stmt, &last_insert_id, &rows_affected);
		if (rc == SQLITE_OK) {
			ctx->response.type = DQLITE_RESPONSE_RESULT;
			ctx->response.result.last_insert_id = last_insert_id;
			ctx->response.result.rows_affected = rows_affected;
		} else {
			dqlite__error_printf(&g->error, stmt->error);
			gateway__failure(g, ctx, rc);
			goto err;
		}

		sql = stmt->tail;

		/* Ignore errors here. TODO: can this fail? */
		db__finalize(db, stmt);
	}

	return;

err:
	/* Ignore errors here. TODO: emit a warning instead */
	db__finalize(db, stmt);
}

static void gateway__query_sql(struct gateway *g, struct gateway__ctx *ctx)
{
	int rc;
	struct db *db;
	struct stmt *stmt;

	GATEWAY__BARRIER;
	GATEWAY__LOOKUP_DB(ctx->request->query_sql.db_id);

	assert(db != NULL);

	rc = db__prepare(db, ctx->request->query_sql.sql, &stmt);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error, db->error);
		gateway__failure(g, ctx, rc);
		return;
	}

	rc = stmt__bind(stmt, &ctx->request->message);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&g->error, stmt->error);
		gateway__failure(g, ctx, rc);
		return;
	}

	/* When the request is completed, the statement needs to be
	 * finalized. */
	ctx->cleanup = GATEWAY__CLEANUP_FINALIZE;

	gateway__query_batch(g, db, stmt, ctx);
}

static void gateway__interrupt(struct gateway *g, struct gateway__ctx *ctx)
{
	assert(g != NULL);
	assert(ctx != NULL);

	/* If there's no ongoing database request there's really nothing to
	 * do. */
	if (g->ctxs[0].request == NULL) {
		goto out;
	}

	assert(g->ctxs[0].cleanup == GATEWAY__CLEANUP_NONE ||
	       g->ctxs[0].cleanup == GATEWAY__CLEANUP_FINALIZE);

	/* Take appropriate action depending on the cleanup code. */
	switch (g->ctxs[0].cleanup) {
		case GATEWAY__CLEANUP_NONE:
			/* Nothing to do */
			break;
		case GATEWAY__CLEANUP_FINALIZE:
			/* Finalize the statempt */
			db__finalize(g->ctxs[0].db, g->ctxs[0].stmt);
			break;
	}

out:
	g->ctxs[0].request = NULL;
	g->ctxs[0].db = NULL;
	g->ctxs[0].stmt = NULL;
	g->ctxs[0].cleanup = GATEWAY__CLEANUP_NONE;

	ctx->response.type = DQLITE_RESPONSE_EMPTY;
}

/* Dispatch a request to the appropriate request handler. */
static void gateway__dispatch(struct gateway *g, struct gateway__ctx *ctx)
{
	switch (ctx->request->type) {
#define GATEWAY__HANDLE(CODE, STRUCT, NAME, _) \
	case CODE:                             \
		gateway__##NAME(g, ctx);       \
		break;

		REQUEST__SCHEMA_TYPES(GATEWAY__HANDLE, );

		default:
			dqlite__error_printf(&g->error,
					     "invalid request type %d",
					     ctx->request->type);
			gateway__failure(g, ctx, SQLITE_ERROR);
			break;
	}

	g->callbacks.xFlush(g->callbacks.ctx, &ctx->response);
}

#ifdef DQLITE_EXPERIMENTAL

/* Use a global variable to pass a gateway object as argument of the main loop
 * coroutine entry point. */
static struct gateway *gateway__loop_arg;

static void gateway__loop()
{
	struct gateway *g = gateway__loop_arg;

	/* Pass control back to the main coroutine, as all we need to do
	 * initially is to set the gateway local variable above */
	co_switch(g->main_coroutine);

	while (1) {
		struct gateway__ctx *ctx = &g->ctxs[0];

		assert(ctx->request != NULL);

		gateway__dispatch(g, ctx);

		co_switch(g->main_coroutine);
	}
}

#endif /* DQLITE_EXPERIMENTAL */

void gateway__init(struct gateway *g,
		   struct gateway__cbs *callbacks,
		   struct dqlite_cluster *cluster,
		   struct dqlite_logger *logger,
		   struct dqlite__options *options)
{
	int i;

	assert(g != NULL);
	assert(cluster != NULL);
	assert(logger != NULL);
	assert(options != NULL);
	assert(callbacks != NULL);
	assert(callbacks->xFlush != NULL);

	dqlite__lifecycle_init(DQLITE__LIFECYCLE_GATEWAY);

	g->client_id = 0;

	dqlite__error_init(&g->error);

	/* Make a copy of the callbacks passed as argument. */
	memcpy(&g->callbacks, callbacks, sizeof *callbacks);

	g->cluster = cluster;
	g->logger = logger;
	g->options = options;

	/* Reset all request contexts in the buffer */
	for (i = 0; i < GATEWAY__MAX_REQUESTS; i++) {
		g->ctxs[i].request = NULL;
		g->ctxs[i].db = NULL;
		g->ctxs[i].stmt = NULL;
		g->ctxs[i].cleanup = GATEWAY__CLEANUP_NONE;
		response_init(&g->ctxs[i].response);
	}

	g->db = NULL;

#ifdef DQLITE_EXPERIMENTAL
	g->main_coroutine = NULL;
	g->loop_coroutine = NULL;
#endif /* DQLITE_EXPERIMENTAL */
}

#ifdef DQLITE_EXPERIMENTAL

int gateway__start(struct gateway *g, uint64_t now)
{
	g->heartbeat = now;
	g->main_coroutine = co_active();
	g->loop_coroutine =
	    co_create(1024 * 1024 * sizeof(void *), gateway__loop);

	if (g->loop_coroutine == NULL) {
		return DQLITE_NOMEM;
	}

	gateway__loop_arg = g;

	/* Kick off the gateway loop coroutine, which will initialize itself by
	 * saving a reference to this gateway object and then immediately switch
	 * back here. */
	co_switch(g->loop_coroutine);

	return 0;
}

#endif /* DQLITE_EXPERIMENTAL */

void gateway__close(struct gateway *g)
{
	int i;

	assert(g != NULL);

	if (g->db != NULL) {
		db__close(g->db);
		sqlite3_free(g->db);
	}

#ifdef DQLITE_EXPERIMENTAL

	if (g->loop_coroutine != NULL) {
		co_delete(g->loop_coroutine);
	}

#endif /* DQLITE_EXPERIMENTAL */

	for (i = 0; i < GATEWAY__MAX_REQUESTS; i++) {
		response_close(&g->ctxs[i].response);
	}

	dqlite__error_close(&g->error);

	dqlite__lifecycle_close(DQLITE__LIFECYCLE_GATEWAY);
}

int gateway__ctx_for(struct gateway *g, int type)
{
	int idx;
	assert(g != NULL);

	/* The first slot is reserved for database requests, and the second for
	 * control ones. A control request can be served concurrently with a
	 * database request, but not the other way round. */
	switch (type) {
		case DQLITE_REQUEST_HEARTBEAT:
		case DQLITE_REQUEST_INTERRUPT:
			idx = 1;
			break;
		default:
			if (g->ctxs[1].request != NULL) {
				return -1;
			}
			idx = 0;
			break;
	}

	if (g->ctxs[idx].request == NULL) {
		return idx;
	}

	return -1;
}

int gateway__handle(struct gateway *g, struct request *request)
{
	struct gateway__ctx *ctx;
	int i;
	int err;

	assert(g != NULL);
	assert(request != NULL);

	/* Abort if we can't accept the request at this time */
	i = gateway__ctx_for(g, request->type);
	if (i == -1) {
		dqlite__error_printf(&g->error,
				     "concurrent request limit exceeded");
		err = DQLITE_PROTO;
		goto err;
	}

	/* Save the request in the context object. */
	ctx = &g->ctxs[i];
	ctx->request = request;

	if (i == 1) {
		/* Heartbeat and interrupt requests are handled synchronously.
		 */
		gateway__dispatch(g, ctx);
	} else {
#ifdef DQLITE_EXPERIMENTAL
		/* Database requests are handled asynchronously by the gateway
		 * coroutine. */
		co_switch(g->loop_coroutine);
#else
		gateway__dispatch(g, ctx);
#endif /* DQLITE_EXPERIMENTAL */
	}

	return 0;
err:
	assert(err != 0);

	return err;
}

/* Resume stepping through a query and send a new follow-up response with more
 * rows. */
static void gateway__query_resume(struct gateway *g, struct gateway__ctx *ctx)
{
	assert(ctx->db != NULL);
	assert(ctx->stmt != NULL);

	gateway__query_batch(g, ctx->db, ctx->stmt, ctx);

	/* Notify user code that a response is available. */
	g->callbacks.xFlush(g->callbacks.ctx, &ctx->response);
}

void gateway__flushed(struct gateway *g, struct response *response)
{
	int i;

	assert(g != NULL);
	assert(response != NULL);

	/* Reset the request context associated with this response */
	for (i = 0; i < GATEWAY__MAX_REQUESTS; i++) {
		struct gateway__ctx *ctx = &g->ctxs[i];
		if (&ctx->response == response) {
			reset_response(response);
			if (ctx->stmt != NULL) {
				gateway__query_resume(g, ctx);
			} else {
				ctx->request = NULL;
			}
			break;
		}
	}

	/* Assert that an associated request was indeed found */
	assert(i < GATEWAY__MAX_REQUESTS);
}

void gateway__aborted(struct gateway *g, struct response *response)
{
	assert(g != NULL);
	assert(response != NULL);
}
