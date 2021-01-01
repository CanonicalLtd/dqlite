/* Core dqlite client logic for encoding requests and decoding responses. */

#ifndef CLIENT_H_
#define CLIENT_H_

#include "lib/buffer.h"

#include "tuple.h"

struct client
{
	int fd;		     /* Connected socket */
	unsigned dbId;       /* Database ID provided by the server */
	struct buffer read;  /* Read buffer */
	struct buffer write; /* Write buffer */
};

struct row
{
	struct value *values;
	struct row *next;
};

struct rows
{
	unsigned columnCount;
	const char **columnNames;
	struct row *next;
};

/* Initialize a new client, writing requests to fd. */
int clientInit(struct client *c, int fd);

/* Release all memory used by the client, and close the client socket. */
void clientClose(struct client *c);

/* Initialize the connection by writing the protocol version. This must be
 * called before using any other API. */
int clientSendHandshake(struct client *c);

/* Send a request to open a database */
int clientSendOpen(struct client *c, const char *name);

/* Receive the response to an open request. */
int clientRecvDb(struct client *c);

/* Send a request to prepare a statement. */
int clientSendPrepare(struct client *c, const char *sql);

/* Receive the response to a prepare request. */
int clientRecvStmt(struct client *c, unsigned *stmtId);

/* Send a request to execute a statement. */
int clientSendExec(struct client *c, unsigned stmtId);

/* Send a request to execute a non-prepared statement. */
int clientSendExecSQL(struct client *c, const char *sql);

/* Receive the response to an exec request. */
int clientRecvResult(struct client *c,
		     unsigned *lastInsertId,
		     unsigned *rowsAffected);

/* Send a request to perform a query. */
int clientSendQuery(struct client *c, unsigned stmtId);

/* Receive the response of a query request. */
int clientRecvRows(struct client *c, struct rows *rows);

/* Release all memory used in the given rows object. */
void clientCloseRows(struct rows *rows);

/* Send a request to add a dqlite node. */
int clientSendAdd(struct client *c, unsigned id, const char *address);

/* Send a request to assign a role to a node. */
int clientSendAssign(struct client *c, unsigned id, int role);

/* Send a request to remove a server from the cluster. */
int clientSendRemove(struct client *c, unsigned id);

/* Receive an empty response. */
int clientRecvEmpty(struct client *c);

#endif /* CLIENT_H_*/
