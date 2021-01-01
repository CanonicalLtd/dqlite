#ifndef VFS_H_
#define VFS_H_

#include <sqlite3.h>

#include "config.h"

/* Initialize the given SQLite VFS interface with dqlite's custom
 * implementation. */
int VfsInit(struct sqlite3_vfs *vfs, const char *name);

/* Release all memory associated with the given dqlite in-memory VFS
 * implementation.
 *
 * This function also automatically unregister the implementation from the
 * SQLite global registry. */
void VfsClose(struct sqlite3_vfs *vfs);

/* Check if the last sqlite3_step() call triggered a write transaction, and
 * return its content if so. */
int VfsPoll(sqlite3_vfs *vfs,
	    const char *database,
	    dqlite_vfs_frame **frames,
	    unsigned *n);

/* Append the given frames to the WAL. */
int VfsApply(sqlite3_vfs *vfs,
	     const char *filename,
	     unsigned n,
	     unsigned long *pageNumbers,
	     void *frames);

/* Cancel a pending transaction. */
int VfsAbort(sqlite3_vfs *vfs, const char *filename);

/* Make a full snapshot of a database. */
int VfsSnapshot(sqlite3_vfs *vfs, const char *filename, void **data, size_t *n);

/* Restore a database snapshot. */
int VfsRestore(sqlite3_vfs *vfs,
	       const char *filename,
	       const void *data,
	       size_t n);

#endif /* VFS_H_ */
