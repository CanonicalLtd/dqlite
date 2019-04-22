/**
 * Track the state of leader connection and execute statements asynchronously.
 */

#ifndef LEADER_H_
#define LEADER_H_

#include <stdbool.h>

#include <libco.h>
#include <raft.h>
#include <sqlite3.h>

#include "./lib/queue.h"

#include "db.h"

struct exec;
struct barrier;
struct leader;

typedef void (*exec_cb)(struct exec *req, int status);
typedef void (*barrier_cb)(struct barrier *req, int status);

struct leader
{
	struct db *db;     /* Database the connection is opened against */
	cothread_t main;   /* Main coroutine */
	cothread_t loop;   /* Leader loop coroutine, executing statements */
	sqlite3 *conn;     /* Underlying SQLite connection */
	struct raft *raft; /* Raft instance */
	struct exec *exec; /* Exec request currently in progress, if any */
	queue queue;       /* Prev/next leader connection, used by the db */
};

struct barrier
{
	void *data;
	struct leader *leader;
	struct raft_barrier req;
	barrier_cb cb;
};

/**
 * Asynchronous request to execute a statement.
 */
struct exec
{
	void *data;
	struct leader *leader;
	struct barrier barrier;
	sqlite3_stmt *stmt;
	bool done;
	int status;
	queue queue;
	exec_cb cb;
};

/**
 * Initialize a new leader connection.
 *
 * This function will start the leader loop coroutine and pause it immediately,
 * transfering control back to main coroutine and then opening a new leader
 * connection against the given database.
 */
int leader__init(struct leader *l, struct db *db, struct raft *raft);

void leader__close(struct leader *l);

/**
 * Submit a request to step a SQLite statement.
 *
 * The request will be dispatched to the leader loop coroutine, which will be
 * resumed and will invoke sqlite_step(). If the statement triggers the
 * replication hooks and one or more new Raft log entries need to be appended,
 * then the loop coroutine will be paused and control will be transferred back
 * to the main coroutine. In this state the leader loop coroutine call stack
 * will be "blocked" on the xFrames() replication hook call triggered by the top
 * sqlite_step() call. The leader loop coroutine will be resumed once the Raft
 * append request completes (either successfully or not) and at that point the
 * stack will rewind back to the @sqlite_step() call, returning to the leader
 * loop which will then have completed the request and transfer control back to
 * the main coroutine, pausing until the next request.
 */
int leader__exec(struct leader *l,
		 struct exec *req,
		 sqlite3_stmt *stmt,
		 exec_cb cb);

/**
 * Submit a raft barrier request if there is no transaction in progress in the
 * underlying database and the FSM is behind the last log index.
 *
 * Otherwise, just invoke the given @cb immediately.
 */
int leader__barrier(struct leader *l, struct barrier *barrier, barrier_cb cb);

#endif /* LEADER_H_*/
