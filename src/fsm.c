#include <raft.h>

#include "lib/assert.h"
#include "lib/serialize.h"

#include "command.h"
#include "fsm.h"
#include "vfs.h"

struct fsm
{
	struct logger *logger;
	struct registry *registry;
	struct
	{
		unsigned nPages;
		unsigned long *pageNumbers;
		uint8_t *pages;
	} pending; /* For upgrades from V1 */
};

static int applyOpen(struct fsm *f, const struct commandopen *c)
{
	(void)f;
	(void)c;
	return 0;
}

static int addPendingPages(struct fsm *f,
			   unsigned long *pageNumbers,
			   uint8_t *pages,
			   unsigned nPages,
			   unsigned pageSize)
{
	unsigned n = f->pending.nPages + nPages;
	unsigned i;

	f->pending.pageNumbers = sqlite3_realloc64(
	    f->pending.pageNumbers, n * sizeof *f->pending.pageNumbers);

	if (f->pending.pageNumbers == NULL) {
		return DQLITE_NOMEM;
	}

	f->pending.pages = sqlite3_realloc64(f->pending.pages, n * pageSize);

	if (f->pending.pages == NULL) {
		return DQLITE_NOMEM;
	}

	for (i = 0; i < nPages; i++) {
		unsigned j = f->pending.nPages + i;
		f->pending.pageNumbers[j] = pageNumbers[i];
		memcpy(f->pending.pages + j * pageSize,
		       (uint8_t *)pages + i * pageSize, pageSize);
	}
	f->pending.nPages = n;

	return 0;
}

static int applyFrames(struct fsm *f, const struct commandframes *c)
{
	struct db *db;
	sqlite3_vfs *vfs;
	unsigned long *pageNumbers;
	void *pages;
	int exists;
	int rv;

	rv = registryDbGet(f->registry, c->filename, &db);
	if (rv != 0) {
		return rv;
	}

	vfs = sqlite3_vfs_find(db->config->name);

	/* Check if the database file exists, and create it by opening a
	 * connection if it doesn't. */
	rv = vfs->xAccess(vfs, c->filename, 0, &exists);
	assert(rv == 0);

	if (!exists) {
		rv = dbOpenFollower(db);
		if (rv != 0) {
			return rv;
		}
		sqlite3_close(db->follower);
		db->follower = NULL;
	}

	rv = commandFramesPageNumbers(c, &pageNumbers);
	if (rv != 0) {
		return rv;
	}

	commandFramesPages(c, &pages);

	/* If the commit marker is set, we apply the changes directly to the
	 * VFS. Otherwise, if the commit marker is not set, this must be an
	 * upgrade from V1, we accumulate uncommitted frames in memory until the
	 * final commit or a rollback. */
	if (c->isCommit) {
		if (f->pending.nPages > 0) {
			rv = addPendingPages(f, pageNumbers, pages,
					     c->frames.nPages,
					     db->config->pageSize);
			if (rv != 0) {
				return DQLITE_NOMEM;
			}
			rv = VfsApply(vfs, db->filename, f->pending.nPages,
				      f->pending.pageNumbers, f->pending.pages);
			if (rv != 0) {
				return rv;
			}
			sqlite3_free(f->pending.pageNumbers);
			sqlite3_free(f->pending.pages);
			f->pending.nPages = 0;
			f->pending.pageNumbers = NULL;
			f->pending.pages = NULL;
		} else {
			rv = VfsApply(vfs, db->filename, c->frames.nPages,
				      pageNumbers, pages);
			if (rv != 0) {
				return rv;
			}
		}
	} else {
		rv = addPendingPages(f, pageNumbers, pages, c->frames.nPages,
				     db->config->pageSize);
		if (rv != 0) {
			return DQLITE_NOMEM;
		}
	}

	sqlite3_free(pageNumbers);

	return 0;
}

static int applyUndo(struct fsm *f, const struct commandundo *c)
{
	(void)c;

	if (f->pending.nPages == 0) {
		return 0;
	}

	sqlite3_free(f->pending.pageNumbers);
	sqlite3_free(f->pending.pages);
	f->pending.nPages = 0;
	f->pending.pageNumbers = NULL;
	f->pending.pages = NULL;

	return 0;
}

static int applyCheckpoint(struct fsm *f, const struct commandcheckpoint *c)
{
	struct db *db;
	struct sqlite3_file *file;
	int size;
	int ckpt;
	int rv;

	rv = registryDbGet(f->registry, c->filename, &db);
	assert(rv == 0); /* We have registered this filename before. */

	/* Use a new connection to force re-opening the WAL. */
	rv = dbOpenFollower(db);
	if (rv != 0) {
		return rv;
	}

	rv = sqlite3_file_control(db->follower, "main",
				  SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rv == SQLITE_OK); /* Should never fail */

	/* If there's a checkpoint lock in place, we must be the node that
	 * originated the checkpoint command in the first place, let's release
	 * it. */
	file->pMethods->xShmLock(file, 1 /* checkpoint lock */, 1,
				 SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE);

	rv = sqlite3_wal_checkpoint_v2(
	    db->follower, "main", SQLITE_CHECKPOINT_TRUNCATE, &size, &ckpt);
	if (rv != 0) {
		return rv;
	}

	sqlite3_close(db->follower);
	db->follower = NULL;

	/* Since no reader transaction is in progress, we must be able to
	 * checkpoint the entire WAL */
	assert(size == 0);
	assert(ckpt == 0);

	return 0;
}

static int fsmApply(struct raft_fsm *fsm,
		    const struct raft_buffer *buf,
		    void **result)
{
	struct fsm *f = fsm->data;
	int type;
	void *command;
	int rc;
	rc = commandDecode(buf, &type, &command);
	if (rc != 0) {
		// errorf(f->logger, "fsm: decode command: %d", rc);
		goto err;
	}

	switch (type) {
		case COMMAND_OPEN:
			rc = applyOpen(f, command);
			break;
		case COMMAND_FRAMES:
			rc = applyFrames(f, command);
			break;
		case COMMAND_UNDO:
			rc = applyUndo(f, command);
			break;
		case COMMAND_CHECKPOINT:
			rc = applyCheckpoint(f, command);
			break;
		default:
			rc = RAFT_MALFORMED;
			goto errAfterCommandDecode;
	}
	raft_free(command);

	*result = NULL;

	return 0;

errAfterCommandDecode:
	raft_free(command);
err:
	return rc;
}

#define SNAPSHOT_FORMAT 1

#define SNAPSHOT_HEADER(X, ...)          \
	X(uint64, format, ##__VA_ARGS__) \
	X(uint64, n, ##__VA_ARGS__)
SERIALIZE_DEFINE(snapshotHeader, SNAPSHOT_HEADER);
SERIALIZE_IMPLEMENT(snapshotHeader, SNAPSHOT_HEADER);

#define SNAPSHOT_DATABASE(X, ...)          \
	X(text, filename, ##__VA_ARGS__)   \
	X(uint64, mainSize, ##__VA_ARGS__) \
	X(uint64, walSize, ##__VA_ARGS__)
SERIALIZE_DEFINE(snapshotDatabase, SNAPSHOT_DATABASE);
SERIALIZE_IMPLEMENT(snapshotDatabase, SNAPSHOT_DATABASE);

/* Encode the global snapshot header. */
static int encodeSnapshotHeader(unsigned n, struct raft_buffer *buf)
{
	struct snapshotHeader header;
	void *cursor;
	header.format = SNAPSHOT_FORMAT;
	header.n = n;
	buf->len = snapshotHeaderSizeof(&header);
	buf->base = raft_malloc(buf->len);
	if (buf->base == NULL) {
		return RAFT_NOMEM;
	}
	cursor = buf->base;
	snapshotHeaderEncode(&header, &cursor);
	return 0;
}

/* Encode the given database. */
static int encodeDatabase(struct db *db, struct raft_buffer bufs[2])
{
	struct snapshotDatabase header;
	sqlite3_vfs *vfs;
	uint32_t databaseSize = 0;
	uint8_t *page;
	void *cursor;
	int rv;

	header.filename = db->filename;

	vfs = sqlite3_vfs_find(db->config->name);
	rv = VfsSnapshot(vfs, db->filename, &bufs[1].base, &bufs[1].len);
	if (rv != 0) {
		goto err;
	}

	/* Extract the database size from the first page. */
	page = bufs[1].base;
	databaseSize += (uint32_t)(page[28] << 24);
	databaseSize += (uint32_t)(page[29] << 16);
	databaseSize += (uint32_t)(page[30] << 8);
	databaseSize += (uint32_t)(page[31]);

	header.mainSize = databaseSize * db->config->pageSize;
	header.walSize = bufs[1].len - header.mainSize;

	/* Database header. */
	bufs[0].len = snapshotDatabaseSizeof(&header);
	bufs[0].base = raft_malloc(bufs[0].len);
	if (bufs[0].base == NULL) {
		rv = RAFT_NOMEM;
		goto errAfterSnapshot;
	}
	cursor = bufs[0].base;
	snapshotDatabaseEncode(&header, &cursor);

	return 0;

errAfterSnapshot:
	raft_free(bufs[1].base);
err:
	assert(rv != 0);
	return rv;
}

/* Decode the database contained in a snapshot. */
static int decodeDatabase(struct fsm *f, struct cursor *cursor)
{
	struct snapshotDatabase header;
	struct db *db;
	sqlite3_vfs *vfs;
	size_t n;
	int exists;
	int rv;

	rv = snapshotDatabaseDecode(cursor, &header);
	if (rv != 0) {
		return rv;
	}
	rv = registryDbGet(f->registry, header.filename, &db);
	if (rv != 0) {
		return rv;
	}

	vfs = sqlite3_vfs_find(db->config->name);

	/* Check if the database file exists, and create it by opening a
	 * connection if it doesn't. */
	rv = vfs->xAccess(vfs, header.filename, 0, &exists);
	assert(rv == 0);

	if (!exists) {
		rv = dbOpenFollower(db);
		if (rv != 0) {
			return rv;
		}
		sqlite3_close(db->follower);
		db->follower = NULL;
	}

	n = header.mainSize + header.walSize;
	rv = VfsRestore(vfs, db->filename, cursor->p, n);
	if (rv != 0) {
		return rv;
	}
	cursor->p += n;

	return 0;
}

static int fsmSnapshot(struct raft_fsm *fsm,
		       struct raft_buffer *bufs[],
		       unsigned *nBufs)
{
	struct fsm *f = fsm->data;
	queue *head;
	struct db *db;
	unsigned n = 0;
	unsigned i;
	int rv;

	/* First count how many databases we have and check that no transaction
	 * is in progress. */
	QUEUE_FOREACH(head, &f->registry->dbs)
	{
		db = QUEUE_DATA(head, struct db, queue);
		if (db->txId != 0) {
			return RAFT_BUSY;
		}
		n++;
	}

	*nBufs = 1;      /* Snapshot header */
	*nBufs += n * 2; /* Database header an content */
	*bufs = raft_malloc(*nBufs * sizeof **bufs);
	if (*bufs == NULL) {
		rv = RAFT_NOMEM;
		goto err;
	}

	rv = encodeSnapshotHeader(n, &(*bufs)[0]);
	if (rv != 0) {
		goto errAfterBufsAlloc;
	}

	/* Encode individual databases. */
	i = 1;
	QUEUE_FOREACH(head, &f->registry->dbs)
	{
		db = QUEUE_DATA(head, struct db, queue);
		rv = encodeDatabase(db, &(*bufs)[i]);
		if (rv != 0) {
			goto errAfterEncodeHeader;
		}
		i += 2;
	}

	return 0;

errAfterEncodeHeader:
	do {
		raft_free((*bufs)[i].base);
		i--;
	} while (i > 0);
errAfterBufsAlloc:
	raft_free(*bufs);
err:
	assert(rv != 0);
	return rv;
}

static int fsmRestore(struct raft_fsm *fsm, struct raft_buffer *buf)
{
	struct fsm *f = fsm->data;
	struct cursor cursor = {buf->base, buf->len};
	struct snapshotHeader header;
	unsigned i;
	int rv;

	rv = snapshotHeaderDecode(&cursor, &header);
	if (rv != 0) {
		return rv;
	}
	if (header.format != SNAPSHOT_FORMAT) {
		return RAFT_MALFORMED;
	}

	for (i = 0; i < header.n; i++) {
		rv = decodeDatabase(f, &cursor);
		if (rv != 0) {
			return rv;
		}
	}

	raft_free(buf->base);

	return 0;
}

int fsmInit(struct raft_fsm *fsm,
	    struct config *config,
	    struct registry *registry)
{
	struct fsm *f = raft_malloc(sizeof *f);

	if (f == NULL) {
		return DQLITE_NOMEM;
	}

	f->logger = &config->logger;
	f->registry = registry;
	f->pending.nPages = 0;
	f->pending.pageNumbers = NULL;
	f->pending.pages = NULL;

	fsm->version = 1;
	fsm->data = f;
	fsm->apply = fsmApply;
	fsm->snapshot = fsmSnapshot;
	fsm->restore = fsmRestore;

	return 0;
}

void fsmClose(struct raft_fsm *fsm)
{
	struct fsm *f = fsm->data;
	raft_free(f);
}
