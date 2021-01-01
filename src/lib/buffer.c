#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "buffer.h"

#include "../../include/dqlite.h"

/* How large is the buffer currently */
#define SIZE(B) (B->nPages * B->pageSize)

/* How many remaining bytes the buffer currently */
#define CAP(B) (SIZE(B) - B->offset)

int bufferInit(struct buffer *b)
{
	b->pageSize = (unsigned)sysconf(_SC_PAGESIZE);
	b->nPages = 1;
	b->data = malloc(SIZE(b));
	if (b->data == NULL) {
		return DQLITE_NOMEM;
	}
	b->offset = 0;
	return 0;
}

void bufferClose(struct buffer *b)
{
	free(b->data);
}

/* Ensure that the buffer as at least @size spare bytes */
static bool ensure(struct buffer *b, size_t size)
{
	/* Double the buffer until we have enough capacity */
	while (size > CAP(b)) {
		void *data;
		b->nPages *= 2;
		data = realloc(b->data, SIZE(b));
		if (data == NULL) {
			return false;
		}
		b->data = data;
	}
	return true;
}

void *bufferAdvance(struct buffer *b, size_t size)
{
	void *cursor;

	if (!ensure(b, size)) {
		return NULL;
	}

	cursor = bufferCursor(b, b->offset);
	b->offset += size;
	return cursor;
}

size_t bufferOffset(struct buffer *b)
{
	return b->offset;
}

void *bufferCursor(struct buffer *b, size_t offset)
{
	return b->data + offset;
}

void bufferReset(struct buffer *b)
{
	b->offset = 0;
}
