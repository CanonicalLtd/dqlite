#ifndef DQLITE_ERROR_H
#define DQLITE_ERROR_H

#include <string.h>

#include <sqlite3.h>

/* A message describing the last error occurred on an object */
typedef char *dqliteError;

/* Initialize the error with an empty message */
void dqliteErrorInit(dqliteError *e);

/* Release the memory of the error message, if any is set */
void dqliteErrorClose(dqliteError *e);

/* Set the error message */
void dqliteErrorPrintf(dqliteError *e, const char *fmt, ...);

/* Wrap an error with an additional message */
void dqliteErrorWrapf(dqliteError *e,
		      const dqliteError *cause,
		      const char *fmt,
		      ...);

/* Out of memory error */
void dqliteErrorOom(dqliteError *e, const char *msg, ...);

/* Wrap a system error */
void dqliteErrorSys(dqliteError *e, const char *msg);

/* Wrap an error from libuv */
void dqliteErrorUv(dqliteError *e, int err, const char *msg);

/* Copy the underlying error message.
 *
 * Client code is responsible of invoking sqlite3_free to deallocate the
 * returned string.
 */
int dqliteErrorCopy(dqliteError *e, char **msg);

/* Whether the error is not set */
int dqliteErrorIsNull(dqliteError *e);

/* Whether the error is due to client disconnection */
int dqliteErrorIsDisconnect(dqliteError *e);

#endif /* DQLITE_ERROR_H */
