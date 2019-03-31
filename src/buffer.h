/**
 * A dynamic buffer which grows as needed.
 *
 * The buffer size is always a multiple of the OS virtual memory page size, so
 * resizing the buffer *should* not incur in memory being copied.
 *
 * See https://stackoverflow.com/questions/16765389
 *
 * TODO: consider using mremap.
 */

#include <unistd.h>

#ifndef DQLITE_BUFFER_H_
#define DQLITE_BUFFER_H_

struct buffer
{
	void *data;	 /* Allocated buffer */
	unsigned page_size; /* Size of an OS page */
	unsigned n_pages;   /* Number of pages allocated */
	size_t offset;      /* Next byte to write in the buffer */
};

/**
 * Initialize the buffer. It will initially have 1 memory  page.
 */
int buffer__init(struct buffer *b);

/**
 * Release the memory of the buffer.
 */
void buffer__close(struct buffer *b);

/**
 * Return a pointer to the next byte to write, ensuring that the buffer has at
 * least @size spare bytes.
 *
 * Return #NULL in case of out-of-memory errors.
 */
void *buffer__advance(struct buffer *b, size_t size);

/**
 * Reset the write offset of the buffer.
 */
void buffer__reset(struct buffer *b);

#endif /* DQLITE_BUFFER_H_ */
