#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>

#include <raft.h>

#include <sqlite3.h>

#include "../include/dqlite.h"

#include "lib/assert.h"

#include "format.h"
#include "vfs.h"

/* Maximum pathname length supported by this VFS. */
#define VFS__MAX_PATHNAME 512

/* Hold content for a single page or frame in a volatile file. */
struct vfsPage
{
	void *buf; /* Content of the page. */
	void *hdr; /* Page header (only for WAL pages). */
};

/* Create a new volatile page for a database or WAL file.
 *
 * If it's a page for a WAL file, the WAL header will also be allocated. */
static struct vfsPage *vfsPageCreate(int size, int wal)
{
	struct vfsPage *p;

	assert(size > 0);
	assert(wal == 0 || wal == 1);

	p = sqlite3_malloc(sizeof *p);
	if (p == NULL) {
		goto oom;
	}

	p->buf = sqlite3_malloc(size);
	if (p->buf == NULL) {
		goto oom_after_page_alloc;
	}
	memset(p->buf, 0, size);

	if (wal) {
		p->hdr = sqlite3_malloc(FORMAT__WAL_FRAME_HDR_SIZE);
		if (p->hdr == NULL) {
			goto oom_after_buf_malloc;
		}
		memset(p->hdr, 0, FORMAT__WAL_FRAME_HDR_SIZE);
	} else {
		p->hdr = NULL;
	}

	return p;

oom_after_buf_malloc:
	sqlite3_free(p->buf);

oom_after_page_alloc:
	sqlite3_free(p);

oom:
	return NULL;
}

/* Destroy a volatile page */
static void vfsPageDestroy(struct vfsPage *p)
{
	assert(p != NULL);
	assert(p->buf != NULL);

	sqlite3_free(p->buf);

	if (p->hdr != NULL) {
		sqlite3_free(p->hdr);
	}

	sqlite3_free(p);
}

/* Hold content for a shared memory mapping. */
struct vfsShm
{
	void **regions;  /* Pointers to shared memory regions. */
	int regions_len; /* Number of shared memory regions. */

	unsigned shared[SQLITE_SHM_NLOCK];    /* Count of shared locks */
	unsigned exclusive[SQLITE_SHM_NLOCK]; /* Count of exclusive locks */
};

/* Initialize the shared memory mapping of a database file. */
static void vfsShmInit(struct vfsShm *s)
{
	int i;

	s->regions = NULL;
	s->regions_len = 0;

	for (i = 0; i < SQLITE_SHM_NLOCK; i++) {
		s->shared[i] = 0;
		s->exclusive[i] = 0;
	}
}

/* Release all resources used by a shared memory mapping. */
static void vfsShmClose(struct vfsShm *s)
{
	void *region;
	int i;

	assert(s != NULL);

	/* Free all regions. */
	for (i = 0; i < s->regions_len; i++) {
		region = *(s->regions + i);
		assert(region != NULL);
		sqlite3_free(region);
	}

	/* Free the shared memory region array. */
	if (s->regions != NULL) {
		sqlite3_free(s->regions);
	}

	vfsShmInit(s);
}

enum vfsContentType {
	VFS__DATABASE, /* Main database file */
	VFS__JOURNAL,  /* Default SQLite journal file */
	VFS__WAL       /* Write-Ahead Log */
};

/* Hold content for a single file in the volatile file system. */
struct vfsContent
{
	char *filename;           /* Name of the file. */
	struct vfsPage **pages;   /* All pages in the file. */
	unsigned n_pages;         /* Number of pages in the file. */
	unsigned page_size;       /* Page size of each page. */
	unsigned refcount;        /* N. of files referencing this content. */
	enum vfsContentType type; /* Content type (either main db or WAL). */
	union {
		struct /* VFS__DATABASE */
		{
			struct vfsShm shm;      /* Shared memory. */
			struct vfsContent *wal; /* WAL content. */
		};
		struct /* VFS__WAL */
		{
			void *hdr; /* WAL header. */
		};
	};
};

/* Create the content structure for a new volatile file. */
static struct vfsContent *vfsContentCreate(const char *name, int type)
{
	struct vfsContent *c;

	assert(name != NULL);
	assert(type == VFS__DATABASE || type == VFS__JOURNAL ||
	       type == VFS__WAL);

	c = sqlite3_malloc(sizeof *c);
	if (c == NULL) {
		goto oom;
	}

	// Copy the name, since when called from Go, the pointer will be freed.
	c->filename = sqlite3_malloc(strlen(name) + 1);
	if (c->filename == NULL) {
		goto oom_after_content_malloc;
	}
	strcpy(c->filename, name);

	c->pages = 0;
	c->n_pages = 0;
	c->page_size = 0;
	c->refcount = 0;
	c->type = type;

	// For WAL files, also allocate the WAL file header.
	switch (type) {
		case VFS__DATABASE:
			vfsShmInit(&c->shm);
			c->wal = NULL;
			break;
		case VFS__WAL:
			c->hdr = sqlite3_malloc(FORMAT__WAL_HDR_SIZE);
			if (c->hdr == NULL) {
				goto oom_after_filename_malloc;
			}
			memset(c->hdr, 0, FORMAT__WAL_HDR_SIZE);
			break;
	}

	return c;

oom_after_filename_malloc:
	sqlite3_free(c->filename);

oom_after_content_malloc:
	sqlite3_free(c);

oom:
	return NULL;
}

/* Destroy the content of a volatile file. */
static void vfsContentDestroy(struct vfsContent *c)
{
	unsigned i;
	struct vfsPage *page;

	assert(c != NULL);
	assert(c->filename != NULL);

	/* Free the filename. */
	sqlite3_free(c->filename);

	/* Free all pages. */
	for (i = 0; i < c->n_pages; i++) {
		page = *(c->pages + i);
		assert(page != NULL);
		vfsPageDestroy(page);
	}

	/* Free the page array. */
	if (c->pages != NULL) {
		sqlite3_free(c->pages);
	}

	switch (c->type) {
		case VFS__DATABASE:
			vfsShmClose(&c->shm);
		case VFS__JOURNAL:
			break;
		case VFS__WAL:
			assert(c->hdr != NULL);
			sqlite3_free(c->hdr);
			break;
	}

	sqlite3_free(c);
}

/* Return 1 if this file has no content. */
static int vfsContentIsEmpty(struct vfsContent *c)
{
	assert(c != NULL);

	if (c->n_pages == 0) {
		assert(c->pages == NULL);
		return 1;
	}

	// If it was written, a page list and a page size must have been set.
	assert(c->pages != NULL && c->n_pages > 0 && c->page_size > 0);

	return 0;
}

// Get a page from this file, possibly creating a new one.
static int vfsContentPageGet(struct vfsContent *c,
			     int pgno,
			     struct vfsPage **page)
{
	int rc;
	int is_wal;

	assert(c != NULL);
	assert(pgno > 0);

	is_wal = c->type == VFS__WAL;

	/* SQLite should access pages progressively, without jumping more than
	 * one page after the end. */
	if (pgno > (int)(c->n_pages + 1)) {
		rc = SQLITE_IOERR_WRITE;
		goto err;
	}

	if (pgno == (int)(c->n_pages + 1)) {
		/* Create a new page, grow the page array, and append the
		 * new page to it. */
		struct vfsPage **pages; /* New page array. */

		/* We assume that the page size has been set, either by
		 * intercepting the first main database file write, or by
		 * handling a 'PRAGMA page_size=N' command in
		 * vfs__file_control(). This assumption is enforced in
		 * vfsFileWrite(). */
		assert(c->page_size > 0);

		*page = vfsPageCreate(c->page_size, is_wal);
		if (*page == NULL) {
			rc = SQLITE_NOMEM;
			goto err;
		}

		pages = sqlite3_realloc(c->pages, (sizeof *pages) * pgno);
		if (pages == NULL) {
			rc = SQLITE_NOMEM;
			goto err_after_vfs_page_create;
		}

		/* Append the new page to the new page array. */
		*(pages + pgno - 1) = *page;

		/* Update the page array. */
		c->pages = pages;
		c->n_pages = (unsigned)pgno;
	} else {
		/* Return the existing page. */
		assert(c->pages != NULL);
		*page = *(c->pages + pgno - 1);
	}

	return SQLITE_OK;

err_after_vfs_page_create:
	vfsPageDestroy(*page);

err:
	*page = NULL;

	return rc;
}

/* Lookup a page from this file, returning NULL if it doesn't exist. */
static struct vfsPage *vfsContentPageLookup(struct vfsContent *c, int pgno)
{
	struct vfsPage *page;

	assert(c != NULL);
	assert(pgno > 0);

	if (pgno > (int)c->n_pages) {
		/* This page hasn't been written yet. */
		return NULL;
	}

	page = *(c->pages + pgno - 1);

	assert(page != NULL);

	if (c->type == VFS__WAL) {
		assert(page->hdr != NULL);
	}

	return page;
}

/* Truncate the file to be exactly the given number of pages. */
static void vfsContentTruncate(struct vfsContent *content, unsigned n_pages)
{
	struct vfsPage **cursor;
	unsigned i;

	/* We expect callers to only invoke us if some actual content has been
	 * written already. */
	assert(content->n_pages > 0);

	/* Truncate should always shrink a file. */
	assert(n_pages <= content->n_pages);
	assert(content->pages != NULL);

	/* Destroy pages beyond pages_len. */
	cursor = content->pages + n_pages;
	for (i = 0; i < (content->n_pages - n_pages); i++) {
		vfsPageDestroy(*cursor);
		cursor++;
	}

	/* Reset the file header (for WAL files). */
	if (content->type == VFS__WAL) {
		/* We expect callers to always truncate the WAL to zero. */
		assert(n_pages == 0);
		assert(content->hdr != NULL);
		memset(content->hdr, 0, FORMAT__WAL_HDR_SIZE);
	}

	/* Shrink the page array, possibly to 0.
	 *
	 * TODO: in principle realloc could fail also when shrinking. */
	content->pages = sqlite3_realloc(content->pages,
					 (sizeof *(content->pages)) * n_pages);

	/* Update the page count. */
	content->n_pages = n_pages;
}

/* Implementation of the abstract sqlite3_file base class. */
struct vfsFile
{
	sqlite3_file base;          /* Base class. Must be first. */
	struct vfs *vfs;            /* Pointer to volatile VFS data. */
	struct vfsContent *content; /* Handle to the file content. */
	int flags;                  /* Flags passed to xOpen */
	sqlite3_file *temp;         /* For temp-files, actual VFS. */
};

/* Custom dqlite VFS. Contains pointers to the content of all files that were
 * created. */
struct vfs
{
	struct vfsContent **contents; /* Files content */
	unsigned n_contents;          /* Number of files */
	int error;                    /* Last error occurred. */
};

/* Create a new vfs object. */
static struct vfs *vfsCreate()
{
	struct vfs *v;

	v = sqlite3_malloc(sizeof *v);
	if (v == NULL) {
		return NULL;
	}

	v->contents = NULL;
	v->n_contents = 0;

	return v;
}

/* Release the memory used internally by the VFS object.
 *
 * All file content will be de-allocated, so dangling open FDs against
 * those files will be broken.
 */
static void vfsDestroy(struct vfs *r)
{
	unsigned i;

	assert(r != NULL);

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		vfsContentDestroy(content);
	}

	if (r->contents != NULL) {
		sqlite3_free(r->contents);
	}
}

/* Find a content object by filename. */
static struct vfsContent *vfsContentLookup(struct vfs *r, const char *filename)
{
	unsigned i;

	assert(r != NULL);
	assert(filename != NULL);

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		if (strcmp(content->filename, filename) == 0) {
			// Found matching file.
			return content;
		}
	}

	return NULL;
}

/* Find the database content object associated with the given WAL file name. */
static int vfsDatabaseContentLookup(struct vfs *r,
				    const char *wal_filename,
				    struct vfsContent **out)
{
	struct vfsContent *content;
	int main_filename_len;
	char *main_filename;

	assert(r != NULL);
	assert(wal_filename != NULL);
	assert(out != NULL);

	*out = NULL; /* In case of errors */

	main_filename_len = strlen(wal_filename) - strlen("-wal") + 1;
	main_filename = sqlite3_malloc(main_filename_len);

	if (main_filename == NULL) {
		return SQLITE_NOMEM;
	}

	strncpy(main_filename, wal_filename, main_filename_len - 1);
	main_filename[main_filename_len - 1] = '\0';

	content = vfsContentLookup(r, main_filename);

	sqlite3_free(main_filename);

	if (content == NULL) {
		return SQLITE_CORRUPT;
	}

	*out = content;

	return SQLITE_OK;
}

/* Return the size of the database file whose WAL file has the given name.
 *
 * The size must have been previously set when this routine is called. */
static int vfsDatabasePageSize(struct vfs *r,
			       const char *wal_filename,
			       unsigned int *page_size)
{
	struct vfsContent *content;
	int err;

	assert(r != NULL);
	assert(wal_filename != NULL);
	assert(page_size != NULL);

	*page_size = 0; /* In case of errors. */

	err = vfsDatabaseContentLookup(r, wal_filename, &content);
	if (err != SQLITE_OK) {
		return err;
	}

	assert(content->page_size > 0);

	*page_size = content->page_size;

	return SQLITE_OK;
}

static int vfsDeleteContent(struct vfs *r, const char *filename)
{
	unsigned i;

	for (i = 0; i < r->n_contents; i++) {
		struct vfsContent *content = r->contents[i];
		unsigned j;

		if (strcmp(content->filename, filename) != 0) {
			continue;
		}

		/* Check that there are no consumers of this file. */
		if (content->refcount > 0) {
			r->error = EBUSY;
			return SQLITE_IOERR_DELETE;
		}

		/* Free all memory allocated for this file. */
		vfsContentDestroy(content);

		/* Shift all other contents objects. */
		for (j = i + 1; j < r->n_contents; j++) {
			r->contents[j - 1] = r->contents[j];
		}
		r->n_contents--;

		return SQLITE_OK;
	}

	r->error = ENOENT;
	return SQLITE_IOERR_DELETE_NOENT;
}

static int vfsFileClose(sqlite3_file *file)
{
	int rc = SQLITE_OK;
	struct vfsFile *f = (struct vfsFile *)file;
	struct vfs *v = (struct vfs *)(f->vfs);

	if (f->temp != NULL) {
		/* Close the actual temporary file. */
		rc = f->temp->pMethods->xClose(f->temp);
		sqlite3_free(f->temp);

		return rc;
	}

	assert(f->content->refcount);
	f->content->refcount--;

	/* If we got zero references, free the shared memory mapping, if
	 * present. */
	if (f->content->refcount == 0 && f->content->type == VFS__DATABASE) {
		vfsShmClose(&f->content->shm);
	}

	if (f->flags & SQLITE_OPEN_DELETEONCLOSE) {
		rc = vfsDeleteContent(v, f->content->filename);
	}

	return rc;
}

static int vfsFileRead(sqlite3_file *file,
		       void *buf,
		       int amount,
		       sqlite_int64 offset)
{
	struct vfsFile *f = (struct vfsFile *)file;

	int pgno;
	struct vfsPage *page;

	assert(buf != NULL);
	assert(amount > 0);
	assert(f != NULL);

	if (f->temp != NULL) {
		/* Read from the actual temporary file. */
		return f->temp->pMethods->xRead(f->temp, buf, amount, offset);
	}

	assert(f->content != NULL);
	assert(f->content->filename != NULL);
	assert(f->content->refcount > 0);

	/* From SQLite docs:
	 *
	 *   If xRead() returns SQLITE_IOERR_SHORT_READ it must also fill
	 *   in the unread portions of the buffer with zeros.  A VFS that
	 *   fails to zero-fill short reads might seem to work.  However,
	 *   failure to zero-fill short reads will eventually lead to
	 *   database corruption.
	 */

	/* Check if the file is empty. */
	if (vfsContentIsEmpty(f->content)) {
		memset(buf, 0, amount);
		return SQLITE_IOERR_SHORT_READ;
	}

	/* From this point on we can assume that the file was written at least
	 * once. */

	/* Since writes to all files other than the main database or the WAL are
	 * no-ops and the associated content object remains empty, we expect the
	 * content type to be either FORMAT__DB or
	 * FORMAT__WAL. */
	assert(f->content->type == VFS__DATABASE ||
	       f->content->type == VFS__WAL);

	switch (f->content->type) {
		case VFS__DATABASE:
			/* Main database */

			/* If the main database file is not empty, we expect the
			 * page size to have been set by an initial write. */
			assert(f->content->page_size > 0);

			if (offset < f->content->page_size) {
				/* Reading from page 1. We expect the read to be
				 * at most page_size bytes. */
				assert(amount <= (int)f->content->page_size);

				pgno = 1;
			} else {
				/* For pages greater than 1, we expect a full
				 * page read, with an offset that starts exectly
				 * at the page boundary. */
				assert(amount == (int)f->content->page_size);
				assert((offset % f->content->page_size) == 0);

				pgno = (offset / f->content->page_size) + 1;
			}

			assert(pgno > 0);

			page = vfsContentPageLookup(f->content, pgno);

			if (pgno == 1) {
				/* Read the desired part of page 1. */
				memcpy(buf, page->buf + offset, amount);
			} else {
				/* Read the full page. */
				memcpy(buf, page->buf, amount);
			}
			return SQLITE_OK;

		case VFS__JOURNAL:
			return SQLITE_IOERR_READ;

		case VFS__WAL:
			/* WAL file */

			if (f->content->page_size == 0) {
				/* If the page size hasn't been set yet, set it
				 * by copy the one from the associated main
				 * database file. */
				int err = vfsDatabasePageSize(
				    f->vfs, f->content->filename,
				    &f->content->page_size);
				if (err != 0) {
					return err;
				}
			}

			if (offset == 0) {
				/* Read the header. */
				assert(amount == FORMAT__WAL_HDR_SIZE);
				assert(f->content->hdr != NULL);
				memcpy(buf, f->content->hdr,
				       FORMAT__WAL_HDR_SIZE);
				return SQLITE_OK;
			}

			/* For any other frame, we expect either a header read,
			 * a checksum read, a page read or a full frame read. */
			if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
				assert(((offset - FORMAT__WAL_HDR_SIZE) %
					(f->content->page_size +
					 FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
				pgno = format__wal_calc_pgno(
				    f->content->page_size, offset);
			} else if (amount == sizeof(uint32_t) * 2) {
				if (offset == FORMAT__WAL_FRAME_HDR_SIZE) {
					/* Read the checksum from the WAL
					 * header. */
					memcpy(buf, f->content->hdr + offset,
					       amount);
					return SQLITE_OK;
				}
				assert(((offset - 16 - FORMAT__WAL_HDR_SIZE) %
					(f->content->page_size +
					 FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
				pgno = (offset - 16 - FORMAT__WAL_HDR_SIZE) /
					   (f->content->page_size +
					    FORMAT__WAL_FRAME_HDR_SIZE) +
				       1;
			} else if (amount == (int)f->content->page_size) {
				assert(((offset - FORMAT__WAL_HDR_SIZE -
					 FORMAT__WAL_FRAME_HDR_SIZE) %
					(f->content->page_size +
					 FORMAT__WAL_FRAME_HDR_SIZE)) == 0);
				pgno = format__wal_calc_pgno(
				    f->content->page_size, offset);
			} else {
				assert(amount == (FORMAT__WAL_FRAME_HDR_SIZE +
						  (int)f->content->page_size));
				pgno = format__wal_calc_pgno(
				    f->content->page_size, offset);
			}

			if (pgno == 0) {
				// This is an attempt to read a page that was
				// never written.
				memset(buf, 0, amount);
				return SQLITE_IOERR_SHORT_READ;
			}

			page = vfsContentPageLookup(f->content, pgno);

			if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
				memcpy(buf, page->hdr, amount);
			} else if (amount == sizeof(uint32_t) * 2) {
				memcpy(buf, page->hdr + 16, amount);
			} else if (amount == (int)f->content->page_size) {
				memcpy(buf, page->buf, amount);
			} else {
				memcpy(buf, page->hdr,
				       FORMAT__WAL_FRAME_HDR_SIZE);
				memcpy(buf + FORMAT__WAL_FRAME_HDR_SIZE,
				       page->buf, f->content->page_size);
			}

			return SQLITE_OK;
	}

	return SQLITE_IOERR_READ;
}

static int vfsFileWrite(sqlite3_file *file,
			const void *buf,
			int amount,
			sqlite_int64 offset)
{
	struct vfsFile *f = (struct vfsFile *)file;

	unsigned pgno;
	struct vfsPage *page;
	int rc;

	assert(buf != NULL);
	assert(amount > 0);
	assert(f != NULL);

	if (f->temp != NULL) {
		/* Write to the actual temporary file. */
		return f->temp->pMethods->xWrite(f->temp, buf, amount, offset);
	}

	assert(f->content != NULL);
	assert(f->content->filename != NULL);
	assert(f->content->refcount > 0);

	switch (f->content->type) {
		case VFS__DATABASE:
			/* Main database. */
			if (offset == 0) {
				unsigned int page_size;

				/* This is the first database page. We expect
				 * the data to contain at least the header. */
				assert(amount >= FORMAT__DB_HDR_SIZE);

				/* Extract the page size from the header. */
				rc = format__get_page_size(FORMAT__DB, buf,
							   &page_size);
				if (rc != SQLITE_OK) {
					return rc;
				}

				if (f->content->page_size > 0) {
					/* Check that the given page size
					 * actually matches what we have
					 * recorded. Since we make 'PRAGMA
					 * page_size=N' fail if the page is
					 * already set (see struct
					 * vfs__fileControl), there should be
					 * no way for the user to change it. */
					assert(page_size ==
					       f->content->page_size);
				} else {
					/* This must be the very first write to
					 * the
					 * database. Keep track of the page
					 * size. */
					f->content->page_size = page_size;
				}

				pgno = 1;
			} else {
				/* The header must have been written and the
				 * page size set. */
				if (f->content->page_size == 0) {
					return SQLITE_IOERR_WRITE;
				}

				/* For pages beyond the first we expect offset
				 * to be a multiple of the page size. */
				assert((offset % f->content->page_size) == 0);

				/* We expect that SQLite writes a page at time.
				 */
				assert(amount == (int)f->content->page_size);

				pgno = (offset / f->content->page_size) + 1;
			}

			rc = vfsContentPageGet(f->content, pgno, &page);
			if (rc != SQLITE_OK) {
				return rc;
			}

			assert(page->buf != NULL);

			memcpy(page->buf, buf, amount);

			return SQLITE_OK;

		case VFS__JOURNAL:
			// Silently swallow writes to any other file.
			return SQLITE_OK;

		case VFS__WAL:
			/* WAL file. */

			if (f->content->page_size == 0) {
				/* If the page size hasn't been set yet, set it
				 * by copy the one from the associated main
				 * database file. */
				int err = vfsDatabasePageSize(
				    f->vfs, f->content->filename,
				    &f->content->page_size);
				if (err != 0) {
					return err;
				}
			}

			if (offset == 0) {
				/* This is the WAL header. */
				unsigned int page_size;
				int rc;

				/* We expect the data to contain exactly 32
				 * bytes. */
				assert(amount == FORMAT__WAL_HDR_SIZE);

				/* The page size indicated in the header must be
				 * valid
				 * and match the one of the database file. */
				rc = format__get_page_size(FORMAT__WAL, buf,
							   &page_size);
				if (rc != SQLITE_OK) {
					return SQLITE_CORRUPT;
				}

				if (page_size != f->content->page_size) {
					return SQLITE_CORRUPT;
				}

				memcpy(f->content->hdr, buf, amount);
				return SQLITE_OK;
			}

			assert(f->content->page_size > 0);

			/* This is a WAL frame write. We expect either a frame
			 * header or page write. */
			if (amount == FORMAT__WAL_FRAME_HDR_SIZE) {
				/* Frame header write. */
				assert(((offset - FORMAT__WAL_HDR_SIZE) %
					(f->content->page_size +
					 FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

				pgno = format__wal_calc_pgno(
				    f->content->page_size, offset);

				vfsContentPageGet(f->content, pgno, &page);
				if (page == NULL) {
					return SQLITE_NOMEM;
				}
				memcpy(page->hdr, buf, amount);
			} else {
				/* Frame page write. */
				assert(amount == (int)f->content->page_size);
				assert(((offset - FORMAT__WAL_HDR_SIZE -
					 FORMAT__WAL_FRAME_HDR_SIZE) %
					(f->content->page_size +
					 FORMAT__WAL_FRAME_HDR_SIZE)) == 0);

				pgno = format__wal_calc_pgno(
				    f->content->page_size, offset);

				// The header for the this frame must already
				// have been written, so the page is there.
				page = vfsContentPageLookup(f->content, pgno);

				assert(page != NULL);

				memcpy(page->buf, buf, amount);
			}

			return SQLITE_OK;
	}

	return SQLITE_IOERR_WRITE;
}

static int vfsFileTruncate(sqlite3_file *file, sqlite_int64 size)
{
	struct vfsFile *f = (struct vfsFile *)file;
	int pgno;

	assert(f != NULL);
	assert(f->content != NULL);

	/* We expect calls to xTruncate only for database and WAL files. */
	if (f->content->type != VFS__DATABASE && f->content->type != VFS__WAL) {
		return SQLITE_IOERR_TRUNCATE;
	}

	/* Check if this file empty.*/
	if (vfsContentIsEmpty(f->content)) {
		if (size > 0) {
			return SQLITE_IOERR_TRUNCATE;
		}

		/* Nothing to do. */
		return SQLITE_OK;
	}

	switch (f->content->type) {
		case VFS__DATABASE:
			/* Main database. */

			/* Since the file size is not zero, some content must
			 * have been written and the page size must be known. */
			assert(f->content->page_size > 0);

			if ((size % f->content->page_size) != 0) {
				return SQLITE_IOERR_TRUNCATE;
			}

			pgno = size / f->content->page_size;
			break;

		case VFS__WAL:
			/* WAL file. */

			/* We expect SQLite to only truncate to zero, after a
			 * full checkpoint.
			 *
			 * TODO: figure out other case where SQLite might
			 * truncate to a different size.
			 */
			if (size != 0) {
				return SQLITE_PROTOCOL;
			}
			pgno = 0;
			break;

		default:
			return SQLITE_IOERR_TRUNCATE;
	}

	vfsContentTruncate(f->content, pgno);

	return SQLITE_OK;
}

static int vfsFileSync(sqlite3_file *file, int flags)
{
	(void)file;
	(void)flags;

	return SQLITE_IOERR_FSYNC;
}

static int vfsFileSize(sqlite3_file *file, sqlite_int64 *size)
{
	struct vfsFile *f = (struct vfsFile *)file;

	/* Check if this file empty. */
	if (vfsContentIsEmpty(f->content)) {
		*size = 0;
		return SQLITE_OK;
	}

	/* Since we don't allow writing any other file, this must be
	 * either a database file or WAL file. */
	assert(f->content->type == VFS__DATABASE ||
	       f->content->type == VFS__WAL);

	/* Since this file is not empty, the page size must have been set. */
	assert(f->content->page_size > 0);

	switch (f->content->type) {
		case VFS__DATABASE:
			*size = f->content->n_pages * f->content->page_size;
			break;

		case VFS__JOURNAL:
			*size = 0;
			break;

		case VFS__WAL:
			/* TODO? here we assume that FileSize() is never invoked
			 * between a header write and a page write. */
			*size =
			    FORMAT__WAL_HDR_SIZE +
			    (f->content->n_pages * (FORMAT__WAL_FRAME_HDR_SIZE +
						    f->content->page_size));
			break;
	}

	return SQLITE_OK;
}

/* Locking a file is a no-op, since no other process has visibility on it. */
static int vfsFileLock(sqlite3_file *file, int lock)
{
	(void)file;
	(void)lock;

	return SQLITE_OK;
}

/* Unlocking a file is a no-op, since no other process has visibility on it. */
static int vfsFileUnlock(sqlite3_file *file, int lock)
{
	(void)file;
	(void)lock;

	return SQLITE_OK;
}

/* We always report that a lock is held. This routine should be used only in
 * journal mode, so it doesn't matter. */
static int vfsFileCheckReservedLock(sqlite3_file *file, int *result)
{
	(void)file;

	*result = 1;
	return SQLITE_OK;
}

/* Handle pragma a pragma file control. See the xFileControl
 * docstring in sqlite.h.in for more details. */
static int vfsFileControlPragma(struct vfsFile *f, char **fnctl)
{
	const char *left;
	const char *right;

	assert(f != NULL);
	assert(fnctl != NULL);

	left = fnctl[1];
	right = fnctl[2];

	assert(left != NULL);

	if (strcmp(left, "page_size") == 0 && right) {
		/* When the user executes 'PRAGMA page_size=N' we save the
		 * size internally.
		 *
		 * The page size must be between 512 and 65536, and be a
		 * power of two. The check below was copied from
		 * sqlite3BtreeSetPageSize in btree.c.
		 *
		 * Invalid sizes are simply ignored, SQLite will do the same.
		 *
		 * It's not possible to change the size after it's set.
		 */
		int page_size = atoi(right);

		if (page_size >= FORMAT__PAGE_SIZE_MIN &&
		    page_size <= FORMAT__PAGE_SIZE_MAX &&
		    ((page_size - 1) & page_size) == 0) {
			if (f->content->page_size &&
			    page_size != (int)f->content->page_size) {
				fnctl[0] =
				    "changing page size is not supported";
				return SQLITE_IOERR;
			}
			f->content->page_size = page_size;
		}
	} else if (strcmp(left, "journal_mode") == 0 && right) {
		/* When the user executes 'PRAGMA journal_mode=x' we ensure
		 * that the desired mode is 'wal'. */
		if (strcasecmp(right, "wal") != 0) {
			fnctl[0] = "only WAL mode is supported";
			return SQLITE_IOERR;
		}
	}

	/* We're returning NOTFOUND here to tell SQLite that we wish it to go on
	 * with its own handling as well. If we returned SQLITE_OK the page size
	 * of the journal mode wouldn't be effectively set, as the processing of
	 * the PRAGMA would stop here. */
	return SQLITE_NOTFOUND;
}

static int vfsFileControl(sqlite3_file *file, int op, void *arg)
{
	struct vfsFile *f = (struct vfsFile *)file;

	switch (op) {
		case SQLITE_FCNTL_PRAGMA:
			return vfsFileControlPragma(f, arg);
	}

	return SQLITE_OK;
}

static int vfsFileSectorSize(sqlite3_file *file)
{
	(void)file;

	return 0;
}

static int vfsFileDeviceCharacteristics(sqlite3_file *file)
{
	(void)file;

	return 0;
}

/* Simulate shared memory by allocating on the C heap. */
static int vfsFileShmMap(sqlite3_file *file, /* Handle open on database file */
			 int region_index,   /* Region to retrieve */
			 int region_size,    /* Size of regions */
			 int extend, /* True to extend file if necessary */
			 void volatile **out /* OUT: Mapped memory */
)
{
	struct vfsFile *f = (struct vfsFile *)file;
	void *region;
	int rc;

	assert(f->content->type == VFS__DATABASE);

	if (f->content->shm.regions != NULL &&
	    region_index < f->content->shm.regions_len) {
		/* The region was already allocated. */
		region = *(f->content->shm.regions + region_index);
		assert(region != NULL);
	} else {
		if (extend) {
			/* We should grow the map one region at a time. */
			assert(region_index == f->content->shm.regions_len);
			region = sqlite3_malloc(region_size);
			if (region == NULL) {
				rc = SQLITE_NOMEM;
				goto err;
			}

			memset(region, 0, region_size);

			f->content->shm.regions = sqlite3_realloc(
			    f->content->shm.regions,
			    sizeof(void *) * (region_index + 1));

			if (f->content->shm.regions == NULL) {
				rc = SQLITE_NOMEM;
				goto err_after_region_malloc;
			}

			*(f->content->shm.regions + region_index) = region;
			f->content->shm.regions_len++;

		} else {
			/* The region was not allocated and we don't have to
			 * extend the map. */
			region = NULL;
		}
	}

	*out = region;

	return SQLITE_OK;

err_after_region_malloc:
	sqlite3_free(region);

err:
	assert(rc != SQLITE_OK);

	*out = NULL;

	return rc;
}

static int vfsFileShmLock(sqlite3_file *file, int ofst, int n, int flags)
{
	struct vfsFile *f;
	int i;

	assert(file != NULL);

	/* Legal values for the offset and the range */
	assert(ofst >= 0 && ofst + n <= SQLITE_SHM_NLOCK);
	assert(n >= 1);
	assert(n == 1 || (flags & SQLITE_SHM_EXCLUSIVE) != 0);

	/* Legal values for the flags.
	 *
	 * See https://sqlite.org/c3ref/c_shm_exclusive.html. */
	assert(flags == (SQLITE_SHM_LOCK | SQLITE_SHM_SHARED) ||
	       flags == (SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE) ||
	       flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED) ||
	       flags == (SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE));

	/* This is a no-op since shared-memory locking is relevant only for
	 * inter-process concurrency. See also the unix-excl branch from
	 * upstream (git commit cda6b3249167a54a0cf892f949d52760ee557129). */

	f = (struct vfsFile *)file;

	assert(f->content != NULL);

	if (flags & SQLITE_SHM_UNLOCK) {
		unsigned *these_locks;
		unsigned *other_locks;

		if (flags & SQLITE_SHM_SHARED) {
			these_locks = f->content->shm.shared;
			other_locks = f->content->shm.exclusive;
		} else {
			these_locks = f->content->shm.exclusive;
			other_locks = f->content->shm.shared;
		}

		for (i = ofst; i < ofst + n; i++) {
			/* Sanity check that no lock of the other type is held
			 * in this region. */
			assert(other_locks[i] == 0);

			/* Only decrease the lock count if it's positive. In
			 * other words releasing a never acquired lock is legal
			 * and idemponent. */
			if (these_locks[i] > 0) {
				these_locks[i]--;
			}
		}
	} else {
		if (flags & SQLITE_SHM_EXCLUSIVE) {
			/* No shared or exclusive lock must be held in the
			 * region. */
			for (i = ofst; i < ofst + n; i++) {
				if (f->content->shm.shared[i] > 0 ||
				    f->content->shm.exclusive[i] > 0) {
					return SQLITE_BUSY;
				}
			}

			for (i = ofst; i < ofst + n; i++) {
				assert(f->content->shm.exclusive[i] == 0);
				f->content->shm.exclusive[i] = 1;
			}
		} else {
			/* No exclusive lock must be held in the region. */
			for (i = ofst; i < ofst + n; i++) {
				if (f->content->shm.exclusive[i] > 0) {
					return SQLITE_BUSY;
				}
			}

			for (i = ofst; i < ofst + n; i++) {
				f->content->shm.shared[i]++;
			}
		}
	}

	return SQLITE_OK;
}

static void vfsFileShmBarrier(sqlite3_file *file)
{
	(void)file;
	/* This is a no-op since we expect SQLite to be compiled with mutex
	 * support (i.e. SQLITE_MUTEX_OMIT or SQLITE_MUTEX_NOOP are *not*
	 * defined, see sqliteInt.h). */
}

static int vfsFileShmUnmap(sqlite3_file *file, int delete_flag)
{
	(void)file;
	(void)delete_flag;
	return SQLITE_OK;
}

static const sqlite3_io_methods vfsFileMethods = {
    2,                             // iVersion
    vfsFileClose,                  // xClose
    vfsFileRead,                   // xRead
    vfsFileWrite,                  // xWrite
    vfsFileTruncate,               // xTruncate
    vfsFileSync,                   // xSync
    vfsFileSize,                   // xFileSize
    vfsFileLock,                   // xLock
    vfsFileUnlock,                 // xUnlock
    vfsFileCheckReservedLock,      // xCheckReservedLock
    vfsFileControl,                // xFileControl
    vfsFileSectorSize,             // xSectorSize
    vfsFileDeviceCharacteristics,  // xDeviceCharacteristics
    vfsFileShmMap,                 // xShmMap
    vfsFileShmLock,                // xShmLock
    vfsFileShmBarrier,             // xShmBarrier
    vfsFileShmUnmap,               // xShmUnmap
    0,
    0,
};

static int vfsOpen(sqlite3_vfs *vfs,
		   const char *filename,
		   sqlite3_file *file,
		   int flags,
		   int *out_flags)
{
	struct vfs *v;
	struct vfsFile *f;
	struct vfsContent *content;
	enum vfsContentType type;
	bool exists;
	int exclusive = flags & SQLITE_OPEN_EXCLUSIVE;
	int create = flags & SQLITE_OPEN_CREATE;
	int rc;

	(void)out_flags;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);
	assert(file != NULL);

	/* From sqlite3.h.in:
	 *
	 *   The SQLITE_OPEN_EXCLUSIVE flag is always used in conjunction with
	 *   the SQLITE_OPEN_CREATE flag, which are both directly analogous to
	 *   the O_EXCL and O_CREAT flags of the POSIX open() API.  The
	 *   SQLITE_OPEN_EXCLUSIVE flag, when paired with the
	 *   SQLITE_OPEN_CREATE, is used to indicate that file should always be
	 *   created, and that it is an error if it already exists.  It is not
	 *   used to indicate the file should be opened for exclusive access.
	 */
	assert(!exclusive || create);

	v = (struct vfs *)(vfs->pAppData);
	f = (struct vfsFile *)file;

	/* This tells SQLite to not call Close() in case we return an error. */
	f->base.pMethods = 0;
	f->temp = NULL;

	/* Save the flags */
	f->flags = flags;

	/* From SQLite documentation:
	 *
	 * If the zFilename parameter to xOpen is a NULL pointer then xOpen
	 * must invent its own temporary name for the file. Whenever the
	 * xFilename parameter is NULL it will also be the case that the
	 * flags parameter will include SQLITE_OPEN_DELETEONCLOSE.
	 */
	if (filename == NULL) {
		assert(flags & SQLITE_OPEN_DELETEONCLOSE);

		/* Open an actual temporary file. */
		vfs = sqlite3_vfs_find("unix");
		assert(vfs != NULL);

		f->temp = sqlite3_malloc(vfs->szOsFile);
		if (f->temp == NULL) {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}
		rc = vfs->xOpen(vfs, NULL, f->temp, flags, out_flags);
		if (rc != SQLITE_OK) {
			sqlite3_free(f->temp);
			return rc;
		}

		f->base.pMethods = &vfsFileMethods;
		f->vfs = NULL;
		f->content = NULL;

		return SQLITE_OK;
	}

	/* Search if the file exists already. */
	content = vfsContentLookup(v, filename);
	exists = content != NULL;

	/* If file exists, and the exclusive flag is on, return an error. */
	if (exists && exclusive && create) {
		v->error = EEXIST;
		rc = SQLITE_CANTOPEN;
		goto err;
	}

	if (!exists) {
		struct vfsContent **contents;
		unsigned n = v->n_contents + 1;

		/* Check the create flag. */
		if (!create) {
			v->error = ENOENT;
			rc = SQLITE_CANTOPEN;
			goto err;
		}

		/* Create a new entry. */
		contents = sqlite3_realloc(v->contents, (sizeof *contents) * n);
		if (contents == NULL) {
			v->error = ENOMEM;
			rc = SQLITE_CANTOPEN;
			goto err;
		}
		v->contents = contents;

		if (flags & SQLITE_OPEN_MAIN_DB) {
			type = VFS__DATABASE;
		} else if (flags & SQLITE_OPEN_MAIN_JOURNAL) {
			type = VFS__JOURNAL;
		} else if (flags & SQLITE_OPEN_WAL) {
			type = VFS__WAL;
		} else {
			v->error = ENOENT;
			return SQLITE_CANTOPEN;
		}

		content = vfsContentCreate(filename, type);
		if (content == NULL) {
			v->error = ENOMEM;
			rc = SQLITE_NOMEM;
			goto err;
		}

		if (type == VFS__WAL) {
			/* An associated database file must have been opened. */
			struct vfsContent *database;
			rc = vfsDatabaseContentLookup(v, filename, &database);
			if (rc != SQLITE_OK) {
				v->error = ENOMEM;
				goto err_after_vfs_content_create;
			}
			database->wal = content;
		}

		v->contents[n - 1] = content;
		v->n_contents = n;
	}

	// Populate the new file handle.
	f->base.pMethods = &vfsFileMethods;
	f->vfs = v;
	f->content = content;

	content->refcount++;

	return SQLITE_OK;

err_after_vfs_content_create:
	vfsContentDestroy(content);

err:
	assert(rc != SQLITE_OK);

	return rc;
}

static int vfsDelete(sqlite3_vfs *vfs, const char *filename, int dir_sync)
{
	struct vfs *v;
	int rc;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);

	v = (struct vfs *)(vfs->pAppData);

	(void)dir_sync;

	rc = vfsDeleteContent(v, filename);

	return rc;
}

static int vfsAccess(sqlite3_vfs *vfs,
		     const char *filename,
		     int flags,
		     int *result)
{
	struct vfs *v;
	struct vfsContent *content;

	assert(vfs != NULL);
	assert(vfs->pAppData != NULL);

	(void)flags;

	v = (struct vfs *)(vfs->pAppData);

	/* If the file exists, access is always granted. */
	content = vfsContentLookup(v, filename);
	if (content == NULL) {
		*result = 0;
	} else {
		*result = 1;
	}

	return SQLITE_OK;
}

static int vfsFullPathname(sqlite3_vfs *vfs,
			   const char *filename,
			   int pathname_len,
			   char *pathname)
{
	(void)vfs;

	// Just return the path unchanged.
	sqlite3_snprintf(pathname_len, pathname, "%s", filename);
	return SQLITE_OK;
}

static void *vfsDlOpen(sqlite3_vfs *vfs, const char *filename)
{
	(void)vfs;
	(void)filename;

	return 0;
}

static void vfsDlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg)
{
	(void)vfs;

	sqlite3_snprintf(nByte, zErrMsg,
			 "Loadable extensions are not supported");
	zErrMsg[nByte - 1] = '\0';
}

static void (*vfsDlSym(sqlite3_vfs *vfs, void *pH, const char *z))(void)
{
	(void)vfs;
	(void)pH;
	(void)z;

	return 0;
}

static void vfsDlClose(sqlite3_vfs *vfs, void *pHandle)
{
	(void)vfs;
	(void)pHandle;

	return;
}

static int vfsRandomness(sqlite3_vfs *vfs, int nByte, char *zByte)
{
	(void)vfs;
	(void)nByte;
	(void)zByte;

	/* TODO (is this needed?) */
	return SQLITE_OK;
}

static int vfsSleep(sqlite3_vfs *vfs, int microseconds)
{
	(void)vfs;

	/* TODO (is this needed?) */
	return microseconds;
}

static int vfsCurrentTimeInt64(sqlite3_vfs *vfs, sqlite3_int64 *piNow)
{
	static const sqlite3_int64 unixEpoch =
	    24405875 * (sqlite3_int64)8640000;
	struct timeval now;

	(void)vfs;

	gettimeofday(&now, 0);
	*piNow =
	    unixEpoch + 1000 * (sqlite3_int64)now.tv_sec + now.tv_usec / 1000;
	return SQLITE_OK;
}

static int vfsCurrentTime(sqlite3_vfs *vfs, double *piNow)
{
	// TODO: check if it's always safe to cast a double* to a
	// sqlite3_int64*.
	return vfsCurrentTimeInt64(vfs, (sqlite3_int64 *)piNow);
}

static int vfsGetLastError(sqlite3_vfs *vfs, int x, char *y)
{
	struct vfs *v = (struct vfs *)(vfs->pAppData);
	int rc;

	(void)vfs;
	(void)x;
	(void)y;

	rc = v->error;

	return rc;
}

int VfsInit(struct sqlite3_vfs *vfs, const char *name)
{
	vfs->iVersion = 2;
	vfs->szOsFile = sizeof(struct vfsFile);
	vfs->mxPathname = VFS__MAX_PATHNAME;
	vfs->pNext = NULL;

	vfs->pAppData = vfsCreate();
	if (vfs->pAppData == NULL) {
		return DQLITE_NOMEM;
	}

	vfs->xOpen = vfsOpen;
	vfs->xDelete = vfsDelete;
	vfs->xAccess = vfsAccess;
	vfs->xFullPathname = vfsFullPathname;
	vfs->xDlOpen = vfsDlOpen;
	vfs->xDlError = vfsDlError;
	vfs->xDlSym = vfsDlSym;
	vfs->xDlClose = vfsDlClose;
	vfs->xRandomness = vfsRandomness;
	vfs->xSleep = vfsSleep;
	vfs->xCurrentTime = vfsCurrentTime;
	vfs->xGetLastError = vfsGetLastError;
	vfs->xCurrentTimeInt64 = vfsCurrentTimeInt64;
	vfs->zName = name;

	sqlite3_vfs_register(vfs, 0);

	return 0;
}

void VfsClose(struct sqlite3_vfs *vfs)
{
	struct vfs *v;
	sqlite3_vfs_unregister(vfs);
	v = (struct vfs *)(vfs->pAppData);
	vfsDestroy(v);
	sqlite3_free(v);
}

/* Guess the file type by looking the filename. */
static int vfsGuessFileType(const char *filename)
{
	/* TODO: improve the check. */
	if (strstr(filename, "-wal") != NULL) {
		return FORMAT__WAL;
	}

	return FORMAT__DB;
}

int VfsFileRead(const char *vfs_name,
		const char *filename,
		void **buf,
		size_t *len)
{
	sqlite3_vfs *vfs;
	int type;
	int flags;
	sqlite3_file *file;
	sqlite3_int64 file_size;
	unsigned page_size;
	sqlite3_int64 offset;
	int rc;

	assert(vfs_name != NULL);
	assert(filename != NULL);
	assert(buf != NULL);
	assert(len != NULL);

	/* Lookup the VFS object to use. */
	vfs = sqlite3_vfs_find(vfs_name);
	if (vfs == NULL) {
		rc = SQLITE_ERROR;
		goto err;
	}

	type = vfsGuessFileType(filename);

	/* Common flags */
	flags = SQLITE_OPEN_READWRITE;

	if (type == FORMAT__DB) {
		flags |= SQLITE_OPEN_MAIN_DB;
	} else {
		flags |= SQLITE_OPEN_WAL;
	}

	/* Open the file */
	file = sqlite3_malloc(vfs->szOsFile);
	if (file == NULL) {
		rc = SQLITE_NOMEM;
		goto err;
	}

	rc = vfs->xOpen(vfs, filename, file, flags, &flags);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Get the file size */
	rc = file->pMethods->xFileSize(file, &file_size);
	if (rc != SQLITE_OK) {
		goto err_after_file_open;
	}
	*len = file_size;

	/* Check if the file is empty. */
	if (*len == 0) {
		*buf = NULL;
		goto out;
	}

	/* Allocate the read buffer.
	 *
	 * TODO: we should fix the tests and use sqlite3_malloc instead. */
	*buf = raft_malloc(*len);
	if (*buf == NULL) {
		rc = SQLITE_NOMEM;
		goto err_after_file_open;
	}

	/* Read the header. The buffer size is enough for both database and WAL
	 * files. */
	rc = file->pMethods->xRead(file, *buf, FORMAT__WAL_HDR_SIZE, 0);
	if (rc != SQLITE_OK) {
		goto err_after_buf_malloc;
	}

	/* Figure the page size. */
	rc = format__get_page_size(type, *buf, &page_size);
	if (rc != SQLITE_OK) {
		goto err_after_buf_malloc;
	}

	offset = 0;

	/* If this is a WAL file , we have already read the header and we can
	 * move on. */
	if (type == FORMAT__WAL) {
		offset += FORMAT__WAL_HDR_SIZE;
	}

	while ((size_t)offset < *len) {
		uint8_t *pos = (*buf) + offset;

		if (type == FORMAT__WAL) {
			/* Read the frame header */
			rc = file->pMethods->xRead(
			    file, pos, FORMAT__WAL_FRAME_HDR_SIZE, offset);
			if (rc != SQLITE_OK) {
				goto err_after_buf_malloc;
			}
			offset += FORMAT__WAL_FRAME_HDR_SIZE;
			pos += FORMAT__WAL_FRAME_HDR_SIZE;
		}

		/* Read the page */
		rc = file->pMethods->xRead(file, pos, page_size, offset);
		if (rc != SQLITE_OK) {
			goto err_after_buf_malloc;
		}
		offset += page_size;
	};

out:
	file->pMethods->xClose(file);
	sqlite3_free(file);

	return SQLITE_OK;

err_after_buf_malloc:
	sqlite3_free(*buf);

err_after_file_open:
	file->pMethods->xClose(file);

err_after_file_malloc:
	sqlite3_free(file);

err:
	assert(rc != SQLITE_OK);

	*buf = NULL;
	*len = 0;

	return rc;
}

int VfsFileWrite(const char *vfs_name,
		 const char *filename,
		 const void *buf,
		 size_t len)
{
	sqlite3_vfs *vfs;
	sqlite3_file *file;
	int type;
	int flags;
	unsigned int page_size;
	sqlite3_int64 offset;
	const uint8_t *pos;
	int rc;

	assert(vfs_name != NULL);
	assert(filename != NULL);
	assert(buf != NULL);
	assert(len > 0);

	/* Lookup the VFS object to use. */
	vfs = sqlite3_vfs_find(vfs_name);
	if (vfs == NULL) {
		rc = SQLITE_ERROR;
		goto err;
	}

	/* Determine if this is a database or a WAL file. */
	type = vfsGuessFileType(filename);

	/* Common flags */
	flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;

	if (type == FORMAT__DB) {
		flags |= SQLITE_OPEN_MAIN_DB;
	} else {
		flags |= SQLITE_OPEN_WAL;
	}

	/* Open the file */
	file = (sqlite3_file *)sqlite3_malloc(vfs->szOsFile);
	if (file == NULL) {
		rc = SQLITE_NOMEM;
		goto err;
	}
	rc = vfs->xOpen(vfs, filename, file, flags, &flags);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Truncate any existing content. */
	rc = file->pMethods->xTruncate(file, 0);
	if (rc != SQLITE_OK) {
		goto err_after_file_malloc;
	}

	/* Figure out the page size */
	rc = format__get_page_size(type, buf, &page_size);
	if (rc != SQLITE_OK) {
		goto err_after_file_open;
	}

	offset = 0;
	pos = buf;

	/* If this is a WAL file , write the header first. */
	if (type == FORMAT__WAL) {
		rc = file->pMethods->xWrite(file, pos, FORMAT__WAL_HDR_SIZE,
					    offset);
		if (rc != SQLITE_OK) {
			goto err_after_file_open;
		}
		offset += FORMAT__WAL_HDR_SIZE;
		pos += FORMAT__WAL_HDR_SIZE;
	}

	while ((size_t)offset < len) {
		if (type == FORMAT__WAL) {
			/* Write the frame header */
			rc = file->pMethods->xWrite(
			    file, pos, FORMAT__WAL_FRAME_HDR_SIZE, offset);
			if (rc != SQLITE_OK) {
				goto err_after_file_open;
			}
			offset += FORMAT__WAL_FRAME_HDR_SIZE;
			pos += FORMAT__WAL_FRAME_HDR_SIZE;
		}

		/* Write the page */
		rc = file->pMethods->xWrite(file, pos, page_size, offset);
		if (rc != SQLITE_OK) {
			goto err_after_file_open;
		}
		offset += page_size;
		pos += page_size;
	};

	file->pMethods->xClose(file);
	sqlite3_free(file);

	return SQLITE_OK;

err_after_file_open:
	file->pMethods->xClose(file);

err_after_file_malloc:
	sqlite3_free(file);

err:
	assert(rc != SQLITE_OK);

	return rc;
}
