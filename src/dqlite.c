#include "../include/dqlite.h"

#include "vfs.h"

int dqlite_vfs_init(sqlite3_vfs *vfs, const char *name)
{
	return VfsInit(vfs, name);
}

void dqlite_vfs_close(sqlite3_vfs *vfs)
{
	VfsClose(vfs);
}

int dqlite_vfs_poll(sqlite3_vfs *vfs,
		    const char *filename,
		    dqlite_vfs_frame **frames,
		    unsigned *n)
{
	return VfsPoll(vfs, filename, frames, n);
}

int dqlite_vfs_apply(sqlite3_vfs *vfs,
		     const char *filename,
		     unsigned n,
		     unsigned long *pageNumbers,
		     void *frames)
{
	return VfsApply(vfs, filename, n, pageNumbers, frames);
}

int dqlite_vfs_abort(sqlite3_vfs *vfs, const char *filename)
{
	return VfsAbort(vfs, filename);
}

int dqlite_vfs_snapshot(sqlite3_vfs *vfs,
			const char *filename,
			void **data,
			size_t *n)
{
	return VfsSnapshot(vfs, filename, data, n);
}

int dqlite_vfs_restore(sqlite3_vfs *vfs,
		       const char *filename,
		       const void *data,
		       size_t n)
{
	return VfsRestore(vfs, filename, data, n);
}
