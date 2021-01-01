#include "gateway.h"

#include "bind.h"
#include "protocol.h"
#include "query.h"
#include "request.h"
#include "response.h"
#include "vfs.h"

void gatewayInit(struct gateway *g,
		 struct config *config,
		 struct registry *registry,
		 struct raft *raft)
{
	g->config = config;
	g->registry = registry;
	g->raft = raft;
	g->leader = NULL;
	g->req = NULL;
	g->stmt = NULL;
	g->stmtFinalize = false;
	g->exec.data = g;
	g->sql = NULL;
	stmtRegistry_init(&g->stmts);
	g->barrier.data = g;
	g->protocol = DQLITE_PROTOCOL_VERSION;
}

void gatewayClose(struct gateway *g)
{
	stmtRegistry_close(&g->stmts);
	if (g->leader != NULL) {
		if (g->stmt != NULL) {
			struct raft_apply *req = &g->leader->inflight->req;
			req->cb(req, RAFT_SHUTDOWN, NULL);
			assert(g->req == NULL);
			assert(g->stmt == NULL);
		}
		leaderClose(g->leader);
		sqlite3_free(g->leader);
	}
}

/* Declare a request struct and a response struct of the appropriate types and
 * decode the request. */
#define START(REQ, RES)                                       \
	struct request##REQ request;                          \
	struct response##RES response;                        \
	{                                                     \
		int rv_;                                      \
		rv_ = request##REQ##Decode(cursor, &request); \
		if (rv_ != 0) {                               \
			return rv_;                           \
		}                                             \
	}

#define CHECK_LEADER(REQ)                                            \
	if (raft_state(req->gateway->raft) != RAFT_LEADER) {         \
		failure(req, SQLITE_IOERR_NOT_LEADER, "not leader"); \
		return 0;                                            \
	}

/* Encode the given success response and invoke the request callback */
#define SUCCESS(LOWER, UPPER)                                                  \
	{                                                                      \
		size_t _n = response##LOWER##Sizeof(&response);                \
		void *_cursor;                                                 \
		assert(_n % 8 == 0);                                           \
		_cursor = bufferAdvance(req->buffer, _n);                      \
		/* Since responses are small and the buffer it's at least 4096 \
		 * bytes, this can't fail. */                                  \
		assert(_cursor != NULL);                                       \
		response##LOWER##Encode(&response, &_cursor);                  \
		req->cb(req, 0, DQLITE_RESPONSE_##UPPER);                      \
	}

/* Lookup the database with the given ID.
 *
 * TODO: support more than one database per connection? */
#define LOOKUP_DB(ID)                                                \
	if (ID != 0 || req->gateway->leader == NULL) {               \
		failure(req, SQLITE_NOTFOUND, "no database opened"); \
		return 0;                                            \
	}

/* Lookup the statement with the given ID. */
#define LOOKUP_STMT(ID)                                    \
	stmt = stmtRegistry_get(&req->gateway->stmts, ID); \
	if (stmt == NULL) {                                \
		failure(req, SQLITE_NOTFOUND,              \
			"no statement with the given id"); \
		return 0;                                  \
	}

#define FAIL_IF_CHECKPOINTING                                                  \
	{                                                                      \
		struct sqlite3_file *_file;                                    \
		int _rv;                                                       \
		_rv = sqlite3_file_control(g->leader->conn, "main",            \
					   SQLITE_FCNTL_FILE_POINTER, &_file); \
		assert(_rv == SQLITE_OK); /* Should never fail */              \
                                                                               \
		_rv = _file->pMethods->xShmLock(                               \
		    _file, 1 /* checkpoint lock */, 1,                         \
		    SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE);                   \
		if (_rv != 0) {                                                \
			assert(_rv == SQLITE_BUSY);                            \
			failure(req, SQLITE_BUSY, "checkpoint in progress");   \
			return 0;                                              \
		}                                                              \
		_file->pMethods->xShmLock(                                     \
		    _file, 1 /* checkpoint lock */, 1,                         \
		    SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE);                 \
	}

/* Encode fa failure response and invoke the request callback */
static void failure(struct handle *req, int code, const char *message)
{
	struct responsefailure failure;
	size_t n;
	void *cursor;
	failure.code = (uint64_t)code;
	failure.message = message;
	n = responsefailureSizeof(&failure);
	assert(n % 8 == 0);
	cursor = bufferAdvance(req->buffer, n);
	/* The buffer has at least 4096 bytes, and error messages are shorter
	 * than that. So this can't fail. */
	assert(cursor != NULL);
	responsefailureEncode(&failure, &cursor);
	req->cb(req, 0, DQLITE_RESPONSE_FAILURE);
}

static int handleLeaderLegacy(struct handle *req, struct cursor *cursor)
{
	START(leader, serverLegacy);
	raft_id id;
	raft_leader(req->gateway->raft, &id, &response.address);
	if (response.address == NULL) {
		response.address = "";
	}
	SUCCESS(serverLegacy, SERVER_LEGACY);
	return 0;
}

static int handle_leader(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	raft_id id = 0;
	const char *address = NULL;
	unsigned i;
	if (g->protocol == DQLITE_PROTOCOL_VERSION_LEGACY) {
		return handleLeaderLegacy(req, cursor);
	}
	START(leader, server);

	/* Only voters might now who the leader is. */
	for (i = 0; i < g->raft->configuration.n; i++) {
		struct raft_server *server = &g->raft->configuration.servers[i];
		if (server->id == g->raft->id && server->role == RAFT_VOTER) {
			raft_leader(req->gateway->raft, &id, &address);
			break;
		}
	}

	response.id = id;
	response.address = address;
	if (response.address == NULL) {
		response.address = "";
	}
	SUCCESS(server, SERVER);
	return 0;
}

static int handle_client(struct handle *req, struct cursor *cursor)
{
	START(client, welcome);
	response.heartbeatTimeout = req->gateway->config->heartbeatTimeout;
	SUCCESS(welcome, WELCOME);
	return 0;
}

static int handle_open(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct db *db;
	int rc;
	START(open, db);
	if (g->leader != NULL) {
		failure(req, SQLITE_BUSY,
			"a database for this connection is already open");
		return 0;
	}
	rc = registryDbGet(g->registry, request.filename, &db);
	if (rc != 0) {
		return rc;
	}
	g->leader = sqlite3_malloc(sizeof *g->leader);
	if (g->leader == NULL) {
		return DQLITE_NOMEM;
	}
	rc = leaderInit(g->leader, db, g->raft);
	if (rc != 0) {
		sqlite3_free(g->leader);
		g->leader = NULL;
		return rc;
	}
	response.id = 0;
	SUCCESS(db, DB);
	return 0;
}

static int handle_prepare(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct stmt *stmt;
	const char *tail;
	int rc;
	START(prepare, stmt);
	LOOKUP_DB(request.dbId);
	rc = stmtRegistry_add(&g->stmts, &stmt);
	if (rc != 0) {
		return rc;
	}
	assert(stmt != NULL);
	rc = sqlite3_prepare_v2(g->leader->conn, request.sql, -1, &stmt->stmt,
				&tail);
	if (rc != SQLITE_OK) {
		failure(req, rc, sqlite3_errmsg(g->leader->conn));
		return 0;
	}
	response.dbId = (uint32_t)request.dbId;
	response.id = (uint32_t)stmt->id;
	response.params = (uint64_t)sqlite3_bind_parameter_count(stmt->stmt);
	SUCCESS(stmt, STMT);
	return 0;
}

/* Fill a result response with the last inserted ID and number of rows
 * affected. */
static void fillResult(struct gateway *g, struct responseresult *response)
{
	response->lastInsertId =
	    (uint64_t)sqlite3_last_insert_rowid(g->leader->conn);
	response->rowsAffected = (uint64_t)sqlite3_changes(g->leader->conn);
}

static const char *errorMessage(sqlite3 *db, int rc)
{
	switch (rc) {
	case SQLITE_IOERR_LEADERSHIP_LOST:
		return "disk I/O error";
	case SQLITE_IOERR_WRITE:
		return "disk I/O error";
	case SQLITE_ABORT:
		return "abort";
	}

	return sqlite3_errmsg(db);
}

static void leaderExecCb(struct exec *exec, int status)
{
	struct gateway *g = exec->data;
	struct handle *req = g->req;
	sqlite3_stmt *stmt = g->stmt;
	struct responseresult response;

	g->req = NULL;
	g->stmt = NULL;

	if (status == SQLITE_DONE) {
		fillResult(g, &response);
		SUCCESS(result, RESULT);
	} else {
		failure(req, status, errorMessage(g->leader->conn, status));
		sqlite3_reset(stmt);
	}
}

static int handle_exec(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct stmt *stmt;
	int rv;
	START(exec, result);
	CHECK_LEADER(req);
	LOOKUP_DB(request.dbId);
	LOOKUP_STMT(request.stmtId);
	FAIL_IF_CHECKPOINTING;
	(void)response;
	rv = bindParams(stmt->stmt, cursor);
	if (rv != 0) {
		failure(req, rv, "bind parameters");
		return 0;
	}
	g->req = req;
	g->stmt = stmt->stmt;
	rv = leaderExec(g->leader, &g->exec, stmt->stmt, leaderExecCb);
	if (rv != 0) {
		return rv;
	}
	return 0;
}

/* Step through the given statement and populate the response buffer of the
 * given request with a single batch of rows.
 *
 * A single batch of rows is typically about the size of a memory page. */
static void gatewayQueryBatch(sqlite3_stmt *stmt, struct handle *req)
{
	struct gateway *g = req->gateway;
	struct responserows response;
	int rc;

	rc = queryBatch(stmt, req->buffer);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		sqlite3_reset(stmt);
		failure(req, rc, sqlite3_errmsg(g->leader->conn));
		goto done;
	}

	if (rc == SQLITE_ROW) {
		response.eof = DQLITE_RESPONSE_ROWS_PART;
		g->req = req;
		g->stmt = stmt;
		SUCCESS(rows, ROWS);
		return;
	} else {
		response.eof = DQLITE_RESPONSE_ROWS_DONE;
		SUCCESS(rows, ROWS);
	}

done:
	if (g->stmtFinalize) {
		/* TODO: do we care about errors? */
		sqlite3_finalize(stmt);
		g->stmtFinalize = false;
	}
	g->stmt = NULL;
	g->req = NULL;
}

static void queryBarrierCb(struct barrier *barrier, int status)
{
	struct gateway *g = barrier->data;
	struct handle *handle = g->req;
	sqlite3_stmt *stmt = g->stmt;

	assert(handle != NULL);
	assert(stmt != NULL);

	g->stmt = NULL;
	g->req = NULL;

	if (status != 0) {
		failure(handle, status, "barrier error");
		return;
	}

	gatewayQueryBatch(stmt, handle);
}

static int handle_query(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct stmt *stmt;
	int rv;
	START(query, rows);
	CHECK_LEADER(req);
	LOOKUP_DB(request.dbId);
	LOOKUP_STMT(request.stmtId);
	FAIL_IF_CHECKPOINTING;
	(void)response;
	rv = bindParams(stmt->stmt, cursor);
	if (rv != 0) {
		failure(req, rv, sqlite3_errmsg(g->leader->conn));
		return 0;
	}
	g->req = req;
	g->stmt = stmt->stmt;
	rv = leaderBarrier(g->leader, &g->barrier, queryBarrierCb);
	if (rv != 0) {
		g->req = NULL;
		g->stmt = NULL;
		return rv;
	}
	return 0;
}

static int handle_finalize(struct handle *req, struct cursor *cursor)
{
	struct stmt *stmt;
	int rv;
	START(finalize, empty);
	LOOKUP_DB(request.dbId);
	LOOKUP_STMT(request.stmtId);
	rv = stmtRegistryDel(&req->gateway->stmts, stmt);
	if (rv != 0) {
		failure(req, rv, "finalize statement");
		return 0;
	}
	SUCCESS(empty, EMPTY);
	return 0;
}

static void handleExecSqlNext(struct handle *req, struct cursor *cursor);

static void handleExecSqlCb(struct exec *exec, int status)
{
	struct gateway *g = exec->data;
	struct handle *req = g->req;

	if (status == SQLITE_DONE) {
		handleExecSqlNext(req, NULL);
	} else {
		failure(req, status, errorMessage(g->leader->conn, status));
		sqlite3_reset(g->stmt);
		sqlite3_finalize(g->stmt);
		g->req = NULL;
		g->stmt = NULL;
		g->sql = NULL;
	}
}

static void handleExecSqlNext(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct responseresult response;
	const char *tail;
	sqlite3_stmt *stmt;
	int rv;

	if (g->sql == NULL || strcmp(g->sql, "") == 0) {
		goto success;
	}

	if (g->stmt != NULL) {
		sqlite3_finalize(g->stmt);
		g->stmt = NULL;
	}

	rv = sqlite3_prepare_v2(g->leader->conn, g->sql, -1, &stmt, &tail);
	if (rv != SQLITE_OK) {
		failure(req, rv, sqlite3_errmsg(g->leader->conn));
		goto done;
	}

	if (stmt == NULL) {
		goto success;
	}

	g->stmt = stmt;

	/* TODO: what about bindings for multi-statement SQL text? */
	if (cursor != NULL) {
		rv = bindParams(stmt, cursor);
		if (rv != SQLITE_OK) {
			failure(req, rv, sqlite3_errmsg(g->leader->conn));
			goto doneAfterPrepare;
		}
	}

	g->sql = tail;
	g->req = req;

	rv = leaderExec(g->leader, &g->exec, g->stmt, handleExecSqlCb);
	if (rv != SQLITE_OK) {
		failure(req, rv, sqlite3_errmsg(g->leader->conn));
		goto doneAfterPrepare;
	}

	return;

success:
	if (g->stmt != NULL) {
		fillResult(g, &response);
	}
	SUCCESS(result, RESULT);

doneAfterPrepare:
	if (g->stmt != NULL) {
		sqlite3_finalize(g->stmt);
	}
done:
	g->req = NULL;
	g->stmt = NULL;
	g->sql = NULL;
}

static int handle_execSql(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	START(execSql, result);
	CHECK_LEADER(req);
	LOOKUP_DB(request.dbId);
	FAIL_IF_CHECKPOINTING;
	(void)response;
	assert(g->req == NULL);
	assert(g->sql == NULL);
	assert(g->stmt == NULL);
	req->gateway->sql = request.sql;
	handleExecSqlNext(req, cursor);
	return 0;
}

static int handle_querySql(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	const char *tail;
	int rv;
	START(querySql, rows);
	CHECK_LEADER(req);
	LOOKUP_DB(request.dbId);
	FAIL_IF_CHECKPOINTING;
	(void)response;
	rv = sqlite3_prepare_v2(g->leader->conn, request.sql, -1, &g->stmt,
				&tail);
	if (rv != SQLITE_OK) {
		failure(req, rv, sqlite3_errmsg(g->leader->conn));
		return 0;
	}
	rv = bindParams(g->stmt, cursor);
	if (rv != 0) {
		sqlite3_finalize(g->stmt);
		g->stmt = NULL;
		failure(req, rv, sqlite3_errmsg(g->leader->conn));
		return 0;
	}
	g->stmtFinalize = true;
	g->req = req;
	rv = leaderBarrier(g->leader, &g->barrier, queryBarrierCb);
	if (rv != 0) {
		g->req = NULL;
		g->stmt = NULL;
		return rv;
	}
	return 0;
}

static int handle_interrupt(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	START(interrupt, empty);

	/* Take appropriate action depending on the cleanup code. */
	if (g->stmtFinalize) {
		sqlite3_finalize(g->stmt);
		g->stmtFinalize = false;
	}
	g->stmt = NULL;
	g->req = NULL;

	SUCCESS(empty, EMPTY);

	return 0;
}

/* Translate a raft error to a dqlite one. */
static int translateRaftErrCode(int code)
{
	switch (code) {
		case RAFT_NOTLEADER:
			return SQLITE_IOERR_NOT_LEADER;
		case RAFT_LEADERSHIPLOST:
			return SQLITE_IOERR_LEADERSHIP_LOST;
		case RAFT_CANTCHANGE:
			return SQLITE_BUSY;
		default:
			return SQLITE_ERROR;
	}
}

struct change
{
	struct gateway *gateway;
	struct raft_change req;
};

static void raftChangeCb(struct raft_change *change, int status)
{
	struct change *r = change->data;
	struct gateway *g = r->gateway;
	struct handle *req = g->req;
	struct responseempty response;
	g->req = NULL;
	sqlite3_free(r);
	if (status != 0) {
		failure(req, translateRaftErrCode(status),
			raft_strerror(status));
	} else {
		SUCCESS(empty, EMPTY);
	}
}

static int handle_add(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct change *r;
	int rv;
	START(add, empty);
	(void)response;

	CHECK_LEADER(req);

	r = sqlite3_malloc(sizeof *r);
	if (r == NULL) {
		return DQLITE_NOMEM;
	}
	r->gateway = g;
	r->req.data = r;

	rv = raft_add(g->raft, &r->req, request.id, request.address,
		      raftChangeCb);
	if (rv != 0) {
		sqlite3_free(r);
		failure(req, translateRaftErrCode(rv), raft_strerror(rv));
		return 0;
	}
	g->req = req;

	return 0;
}

/* Translate a dqlite role code to its raft equivalent. */
static int translateDqliteRole(int role)
{
	switch (role) {
		case DQLITE_VOTER:
			return RAFT_VOTER;
		case DQLITE_STANDBY:
			return RAFT_STANDBY;
		case DQLITE_SPARE:
			return RAFT_SPARE;
		default:
			/* For backward compat with clients that don't set a
			 * role. */
			return DQLITE_VOTER;
	}
}

static int handle_assign(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct change *r;
	uint64_t role = DQLITE_VOTER;
	int rv;
	START(assign, empty);
	(void)response;

	CHECK_LEADER(req);

	/* Detect if this is an assign role request, instead of the former
	 * promote request. */
	if (cursor->cap > 0) {
		rv = uint64Decode(cursor, &role);
		if (rv != 0) {
			return rv;
		}
	}

	r = sqlite3_malloc(sizeof *r);
	if (r == NULL) {
		return DQLITE_NOMEM;
	}
	r->gateway = g;
	r->req.data = r;

	rv = raft_assign(g->raft, &r->req, request.id,
			 translateDqliteRole((int)role), raftChangeCb);
	if (rv != 0) {
		sqlite3_free(r);
		failure(req, translateRaftErrCode(rv), raft_strerror(rv));
		return 0;
	}
	g->req = req;

	return 0;
}

static int handle_remove(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct change *r;
	int rv;
	START(remove, empty);
	(void)response;

	CHECK_LEADER(req);

	r = sqlite3_malloc(sizeof *r);
	if (r == NULL) {
		return DQLITE_NOMEM;
	}
	r->gateway = g;
	r->req.data = r;

	rv = raft_remove(g->raft, &r->req, request.id, raftChangeCb);
	if (rv != 0) {
		sqlite3_free(r);
		failure(req, translateRaftErrCode(rv), raft_strerror(rv));
		return 0;
	}
	g->req = req;

	return 0;
}

static int dumpFile(const char *filename,
		    uint8_t *data,
		    size_t n,
		    struct buffer *buffer)
{
	void *cur;
	uint64_t len = n;

	cur = bufferAdvance(buffer, textSizeof(&filename));
	if (cur == NULL) {
		goto oom;
	}
	textEncode(&filename, &cur);
	cur = bufferAdvance(buffer, uint64Sizeof(&len));
	if (cur == NULL) {
		goto oom;
	}
	uint64Encode(&len, &cur);

	if (n == 0) {
		return 0;
	}

	assert(n % 8 == 0);
	assert(data != NULL);

	cur = bufferAdvance(buffer, n);
	if (cur == NULL) {
		goto oom;
	}
	memcpy(cur, data, n);

	return 0;

oom:
	return DQLITE_NOMEM;
}

static int handle_dump(struct handle *req, struct cursor *cursor)
{
	bool err = true;
	struct gateway *g = req->gateway;
	sqlite3_vfs *vfs;
	void *cur;
	char filename[1024];
	void *data;
	size_t n;
	uint8_t *page;
	uint32_t databaseSize = 0;
	uint8_t *database;
	uint8_t *wal;
	size_t nDatabase;
	size_t nWal;
	int rv;
	START(dump, files);

	response.n = 2;
	cur = bufferAdvance(req->buffer, responsefilesSizeof(&response));
	assert(cur != NULL);
	responsefilesEncode(&response, &cur);

	vfs = sqlite3_vfs_find(g->config->name);
	rv = VfsSnapshot(vfs, request.filename, &data, &n);
	if (rv != 0) {
		failure(req, rv, "failed to dump database");
		return 0;
	}

	if (data != NULL) {
		/* Extract the database size from the first page. */
		page = data;
		databaseSize += (uint32_t)(page[28] << 24);
		databaseSize += (uint32_t)(page[29] << 16);
		databaseSize += (uint32_t)(page[30] << 8);
		databaseSize += (uint32_t)(page[31]);

		nDatabase = databaseSize * g->config->pageSize;
		nWal = n - nDatabase;

		database = data;
		wal = database + nDatabase;
	} else {
		assert(n == 0);

		nDatabase = 0;
		nWal = 0;

		database = NULL;
		wal = NULL;
	}

	rv = dumpFile(request.filename, database, nDatabase, req->buffer);
	if (rv != 0) {
		failure(req, rv, "failed to dump database");
		goto out_free_data;
	}

	strcpy(filename, request.filename);
	strcat(filename, "-wal");
	rv = dumpFile(filename, wal, nWal, req->buffer);
	if (rv != 0) {
		failure(req, rv, "failed to dump wal file");
		goto out_free_data;
	}

	err = false;

out_free_data:
	if (data != NULL) {
		raft_free(data);
	}

	if (!err)
		req->cb(req, 0, DQLITE_RESPONSE_FILES);

	return 0;
}

/* Translate a raft role code to its dqlite equivalent. */
static int translateRaftRole(int role)
{
	switch (role) {
		case RAFT_VOTER:
			return DQLITE_VOTER;
		case RAFT_STANDBY:
			return DQLITE_STANDBY;
		case RAFT_SPARE:
			return DQLITE_SPARE;
		default:
			assert(0);
			return -1;
	}
}

static int encodeServer(struct gateway *g,
			unsigned i,
			struct buffer *buffer,
			int format)
{
	void *cur;
	uint64_t id;
	uint64_t role;
	text_t address;

	id = g->raft->configuration.servers[i].id;
	address = g->raft->configuration.servers[i].address;
	role =
	    (uint64_t)translateRaftRole(g->raft->configuration.servers[i].role);

	cur = bufferAdvance(buffer, uint64Sizeof(&id));
	if (cur == NULL) {
		return DQLITE_NOMEM;
	}
	uint64Encode(&id, &cur);

	cur = bufferAdvance(buffer, textSizeof(&address));
	if (cur == NULL) {
		return DQLITE_NOMEM;
	}
	textEncode(&address, &cur);

	if (format == DQLITE_REQUEST_CLUSTER_FORMAT_V0) {
		return 0;
	}

	cur = bufferAdvance(buffer, uint64Sizeof(&role));
	if (cur == NULL) {
		return DQLITE_NOMEM;
	}
	uint64Encode(&role, &cur);

	return 0;
}

static int handle_cluster(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	unsigned i;
	void *cur;
	int rv;
	START(cluster, servers);

	response.n = g->raft->configuration.n;
	cur = bufferAdvance(req->buffer, responseserversSizeof(&response));
	assert(cur != NULL);
	responseserversEncode(&response, &cur);

	for (i = 0; i < response.n; i++) {
		rv = encodeServer(g, i, req->buffer, (int)request.format);
		if (rv != 0) {
			failure(req, rv, "failed to encode server");
			return 0;
		}
	}

	req->cb(req, 0, DQLITE_RESPONSE_SERVERS);

	return 0;
}

void raftTransferCb(struct raft_transfer *r)
{
	struct gateway *g = r->data;
	struct handle *req = g->req;
	struct responseempty response;
	g->req = NULL;
	sqlite3_free(r);
	if (g->raft->state == RAFT_LEADER) {
		failure(req, DQLITE_ERROR, "leadership transfer failed");
	} else {
		SUCCESS(empty, EMPTY);
	}
}

static int handle_transfer(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	struct raft_transfer *r;
	int rv;
	START(transfer, empty);
	(void)response;

	CHECK_LEADER(req);

	r = sqlite3_malloc(sizeof *r);
	if (r == NULL) {
		return DQLITE_NOMEM;
	}
	r->data = g;

	rv = raft_transfer(g->raft, r, request.id, raftTransferCb);
	if (rv != 0) {
		sqlite3_free(r);
		failure(req, translateRaftErrCode(rv), raft_strerror(rv));
		return 0;
	}
	g->req = req;

	return 0;
}

static int handle_describe(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	START(describe, metadata);
	if (request.format != DQLITE_REQUEST_DESCRIBE_FORMAT_V0) {
		failure(req, SQLITE_PROTOCOL, "bad format version");
	}
	response.failureDomain = g->config->failureDomain;
	response.weight = g->config->weight;
	SUCCESS(metadata, METADATA);
	return 0;
}

static int handle_weight(struct handle *req, struct cursor *cursor)
{
	struct gateway *g = req->gateway;
	START(weight, empty);
	g->config->weight = request.weight;
	SUCCESS(empty, EMPTY);
	return 0;
}

int gatewayHandle(struct gateway *g,
		  struct handle *req,
		  int type,
		  struct cursor *cursor,
		  struct buffer *buffer,
		  handle_cb cb)
{
	int rc = 0;

	/* Check if there is a request in progress. */
	if (g->req != NULL && type != DQLITE_REQUEST_HEARTBEAT) {
		if (g->req->type == DQLITE_REQUEST_QUERY ||
		    g->req->type == DQLITE_REQUEST_QUERY_SQL) {
			/* TODO: handle interrupt requests */
			assert(type == DQLITE_REQUEST_INTERRUPT);
			goto handle;
		}
		if (g->req->type == DQLITE_REQUEST_EXEC ||
		    g->req->type == DQLITE_REQUEST_EXEC_SQL) {
			return SQLITE_BUSY;
		}
		assert(0);
	}

handle:
	req->type = type;
	req->gateway = g;
	req->cb = cb;
	req->buffer = buffer;

	switch (type) {
#define DISPATCH(LOWER, UPPER, _)                 \
	case DQLITE_REQUEST_##UPPER:              \
		rc = handle_##LOWER(req, cursor); \
		break;
		REQUEST_TYPES(DISPATCH);
	}

	return rc;
}

int gatewayResume(struct gateway *g, bool *finished)
{
	if (g->req == NULL || (g->req->type != DQLITE_REQUEST_QUERY &&
			       g->req->type != DQLITE_REQUEST_QUERY_SQL)) {
		*finished = true;
		return 0;
	}
	assert(g->stmt != NULL);
	*finished = false;
	gatewayQueryBatch(g->stmt, g->req);
	return 0;
}
