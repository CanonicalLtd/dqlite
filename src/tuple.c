#include <sqlite3.h>

#include "tuple.h"
#include "assert.h"

/* True if a tuple decoder or decoder is using parameter format. */
#define HAS_PARAMS_FORMAT(P) (P->format == TUPLE_PARAMS)

/* True if a tuple decoder or decoder is using row format. */
#define HAS_ROW_FORMAT(P) (P->format == TUPLE_ROW)

/* Return the tuple header size in bytes, for a tuple of @n values.
 *
 * If the tuple is a row, then each slot is 4 bits, otherwise if the tuple is a
 * sequence of parameters each slot is 8 bits. */
static size_t calcHeaderSize(unsigned n, int format)
{
	size_t size;

	if (format == TUPLE_ROW) {
		size = (n / 2) * sizeof(uint8_t);
		if (n % 2 != 0) {
			size += sizeof(uint8_t);
		}
		size = bytePad64(size);
	} else {
		assert(format == TUPLE_PARAMS);
		/* Include params count for the purpose of calculating possible
		 * padding, but then exclude it as we have already read it. */
		size = sizeof(uint8_t) + n * sizeof(uint8_t);
		size = bytePad64(size);
		size -= sizeof(uint8_t);
	}

	return size;
}

int tupleDecoderInit(struct tupleDecoder *d, unsigned n, struct cursor *cursor)
{
	size_t headerSize;
	int rc;

	d->format = n == 0 ? TUPLE_PARAMS : TUPLE_ROW;

	/* When using row format the number of values is the given one,
	 * otherwise we have to read it from the header. */
	if (HAS_ROW_FORMAT(d)) {
		d->n = n;
	} else {
		uint8_t byte;
		rc = uint8Decode(cursor, &byte);
		if (rc != 0) {
			return rc;
		}
		d->n = byte;
	}

	d->i = 0;
	d->header = cursor->p;

	/* Check that there is enough room to hold n type code slots. */
	headerSize = calcHeaderSize(d->n, d->format);

	if (headerSize > cursor->cap) {
		return DQLITE_PARSE;
	}

	d->cursor = cursor;
	d->cursor->p += headerSize;
	d->cursor->cap -= headerSize;

	return 0;
}

/* Return the number of values in the decoder's tuple. */
unsigned tupleDecoderN(struct tupleDecoder *d)
{
	return d->n;
}

/* Return the type of the i'th value of the tuple. */
static int getType(struct tupleDecoder *d, unsigned i)
{
	int type;

	/* In row format the type slot size is 4 bits, while in params format
	 * the slot is 8 bits. */
	if (HAS_ROW_FORMAT(d)) {
		type = d->header[i / 2];
		if (i % 2 == 0) {
			type &= 0x0f;
		} else {
			type = type >> 4;
		}
	} else {
		type = d->header[i];
	}

	return type;
}

int tupleDecoderNext(struct tupleDecoder *d, struct value *value)
{
	int rc;
	assert(d->i < d->n);
	value->type = getType(d, d->i);
	switch (value->type) {
		case SQLITE_INTEGER:
			rc = int64Decode(d->cursor, &value->integer);
			break;
		case SQLITE_FLOAT:
			rc = floatDecode(d->cursor, &value->float_);
			break;
		case SQLITE_BLOB:
			rc = blobDecode(d->cursor, &value->blob);
			break;
		case SQLITE_NULL:
			/* TODO: allow null to be encoded with 0 bytes? */
			rc = uint64Decode(d->cursor, &value->null);
			break;
		case SQLITE_TEXT:
			rc = textDecode(d->cursor, &value->text);
			break;
		case DQLITE_ISO8601:
			rc = textDecode(d->cursor, &value->iso8601);
			break;
		case DQLITE_BOOLEAN:
			rc = uint64Decode(d->cursor, &value->boolean);
			break;
		default:
			rc = DQLITE_PARSE;
			break;
	};
	if (rc != 0) {
		return rc;
	}
	d->i++;
	return 0;
}

/* Return a pointer to the tuple header. */
static uint8_t *encoderHeader(struct tupleEncoder *e)
{
	return bufferCursor(e->buffer, e->header);
}

int tupleEncoderInit(struct tupleEncoder *e,
		     unsigned n,
		     int format,
		     struct buffer *buffer)
{
	void *cursor;
	size_t nHeader;

	e->n = n;
	e->format = format;
	e->buffer = buffer;
	e->i = 0;

	/* With params format we need to fill the first byte of the header with
	 * the params count. */
	if (HAS_PARAMS_FORMAT(e)) {
		uint8_t *header = bufferAdvance(buffer, 1);
		if (header == NULL) {
			return DQLITE_NOMEM;
		}
		header[0] = (uint8_t)n;
	}

	e->header = bufferOffset(buffer);

	/* Reset the header */
	nHeader = calcHeaderSize(n, format);
	memset(encoderHeader(e), 0, nHeader);

	/* Advance the buffer write pointer past the tuple header. */
	cursor = bufferAdvance(buffer, nHeader);
	if (cursor == NULL) {
		return DQLITE_NOMEM;
	}

	return 0;
}

/* Set the type of the i'th value of the tuple. */
static void setType(struct tupleEncoder *e, unsigned i, int type)
{
	uint8_t *header = encoderHeader(e);

	/* In row format the type slot size is 4 bits, while in params format
	 * the slot is 8 bits. */
	if (HAS_ROW_FORMAT(e)) {
		uint8_t *slot;
		slot = &header[i / 2];
		if (i % 2 == 0) {
			*slot = (uint8_t)type;
		} else {
			*slot |= (uint8_t)(type << 4);
		}
	} else {
		header[i] = (uint8_t)type;
	}
}

int tupleEncoderNext(struct tupleEncoder *e, struct value *value)
{
	void *cursor;
	size_t size;

	assert(e->i < e->n);

	setType(e, e->i, value->type);

	switch (value->type) {
		case SQLITE_INTEGER:
			size = int64Sizeof(&value->integer);
			break;
		case SQLITE_FLOAT:
			size = floatSizeof(&value->float_);
			break;
		case SQLITE_BLOB:
			size = blobSizeof(&value->blob);
			break;
		case SQLITE_NULL:
			/* TODO: allow null to be encoded with 0 bytes */
			size = uint64Sizeof(&value->null);
			break;
		case SQLITE_TEXT:
			size = textSizeof(&value->text);
			break;
		case DQLITE_UNIXTIME:
			size = int64Sizeof(&value->unixtime);
			break;
		case DQLITE_ISO8601:
			size = textSizeof(&value->iso8601);
			break;
		case DQLITE_BOOLEAN:
			size = uint64Sizeof(&value->boolean);
			break;
		default:
			assert(0);
	};

	/* Advance the buffer write pointer. */
	cursor = bufferAdvance(e->buffer, size);
	if (cursor == NULL) {
		return DQLITE_NOMEM;
	}

	switch (value->type) {
		case SQLITE_INTEGER:
			int64Encode(&value->integer, &cursor);
			break;
		case SQLITE_FLOAT:
			floatEncode(&value->float_, &cursor);
			break;
		case SQLITE_BLOB:
			blobEncode(&value->blob, &cursor);
			break;
		case SQLITE_NULL:
			/* TODO: allow null to be encoded with 0 bytes */
			uint64Encode(&value->null, &cursor);
			break;
		case SQLITE_TEXT:
			textEncode(&value->text, &cursor);
			break;
		case DQLITE_UNIXTIME:
			int64Encode(&value->unixtime, &cursor);
			break;
		case DQLITE_ISO8601:
			textEncode(&value->iso8601, &cursor);
			break;
		case DQLITE_BOOLEAN:
			uint64Encode(&value->boolean, &cursor);
			break;
	};

	e->i++;

	return 0;
}
