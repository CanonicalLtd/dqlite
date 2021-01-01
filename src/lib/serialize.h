#ifndef LIB_SERIALIZE_H_
#define LIB_SERIALIZE_H_

#include <stdint.h>
#include <string.h>

#include <uv.h>

#include "../../include/dqlite.h"

#include "assert.h"
#include "byte.h"

#define DQLITE_PARSE 1005

/**
 * The size in bytes of a single serialized word.
 */
#define SERIALIZE_WORD_SIZE 8

/* We rely on the size of double to be 64 bit, since that's what is sent over
 * the wire.
 *
 * See https://stackoverflow.com/questions/752309/ensuring-c-doubles-are-64-bits
 */
#ifndef __STDC_IEC_559__
#if __SIZEOF_DOUBLE__ != 8
#error "Requires IEEE 754 floating point!"
#endif
#endif
#ifdef staticAssert
staticAssert(sizeof(double) == sizeof(uint64_t),
	     "Size of 'double' is not 64 bits");
#endif

/**
 * Basic type aliases to used by macro-based processing.
 */
typedef const char *text_t;
typedef double floatT;
typedef uv_buf_t blobT;

/**
 * Cursor to progressively read a buffer.
 */
struct cursor
{
	const void *p; /* Next byte to read */
	size_t cap;    /* Number of bytes left in the buffer */
};

/**
 * Define a serializable struct.
 *
 * NAME:   Name of the structure which will be defined.
 * FIELDS: List of X-based macros defining the fields in the schema, in the form
 *         of X(KIND, NAME, ##__VA_ARGS__). E.g. X(uint64, id, ##__VA_ARGS__).
 *
 * A new struct called NAME will be defined, along with sizeof, encode and
 * decode functions.
 */
#define SERIALIZE_DEFINE(NAME, FIELDS)         \
	SERIALIZE_DEFINE_STRUCT(NAME, FIELDS); \
	SERIALIZE_DEFINE_METHODS(NAME, FIELDS)

#define SERIALIZE_DEFINE_STRUCT(NAME, FIELDS)  \
	struct NAME                            \
	{                                      \
		FIELDS(SERIALIZE_DEFINE_FIELD) \
	}

#define SERIALIZE_DEFINE_METHODS(NAME, FIELDS)                  \
	size_t NAME##Sizeof(const struct NAME *p);              \
	void NAME##Encode(const struct NAME *p, void **cursor); \
	int NAME##Decode(struct cursor *cursor, struct NAME *p)

/* Define a single field in serializable struct.
 *
 * KIND:   Type code (e.g. uint64, text, etc).
 * MEMBER: Field name. */
#define SERIALIZE_DEFINE_FIELD(KIND, MEMBER) KIND##_t MEMBER;

/**
 * Implement the sizeof, encode and decode function of a serializable struct.
 */
#define SERIALIZE_IMPLEMENT(NAME, FIELDS)                       \
	size_t NAME##Sizeof(const struct NAME *p)               \
	{                                                       \
		size_t size = 0;                                \
		FIELDS(SERIALIZE_SIZEOF_FIELD, p);              \
		return size;                                    \
	}                                                       \
	void NAME##Encode(const struct NAME *p, void **cursor)  \
	{                                                       \
		FIELDS(SERIALIZE_ENCODE_FIELD, p, cursor);      \
	}                                                       \
	int NAME##Decode(struct cursor *cursor, struct NAME *p) \
	{                                                       \
		int rc;                                         \
		FIELDS(SERIALIZE_DECODE_FIELD, p, cursor);      \
		return 0;                                       \
	}

#define SERIALIZE_SIZEOF_FIELD(KIND, MEMBER, P) \
	size += KIND##Sizeof(&((P)->MEMBER));

#define SERIALIZE_ENCODE_FIELD(KIND, MEMBER, P, CURSOR) \
	KIND##Encode(&((P)->MEMBER), CURSOR);

#define SERIALIZE_DECODE_FIELD(KIND, MEMBER, P, CURSOR) \
	rc = KIND##Decode(CURSOR, &((P)->MEMBER));      \
	if (rc != 0) {                                  \
		return rc;                              \
	}

DQLITE_INLINE size_t uint8Sizeof(const uint8_t *value)
{
	(void)value;
	return sizeof(uint8_t);
}

DQLITE_INLINE size_t uint16Sizeof(const uint16_t *value)
{
	(void)value;
	return sizeof(uint16_t);
}

DQLITE_INLINE size_t uint32Sizeof(const uint32_t *value)
{
	(void)value;
	return sizeof(uint32_t);
}

DQLITE_INLINE size_t uint64Sizeof(const uint64_t *value)
{
	(void)value;
	return sizeof(uint64_t);
}

DQLITE_INLINE size_t int64Sizeof(const int64_t *value)
{
	(void)value;
	return sizeof(int64_t);
}

DQLITE_INLINE size_t floatSizeof(const floatT *value)
{
	(void)value;
	return sizeof(double);
}

DQLITE_INLINE size_t textSizeof(const text_t *value)
{
	return bytePad64(strlen(*value) + 1);
}

DQLITE_INLINE size_t blobSizeof(const blobT *value)
{
	return sizeof(uint64_t) /* length */ + bytePad64(value->len) /* data */;
}

DQLITE_INLINE void uint8Encode(const uint8_t *value, void **cursor)
{
	*(uint8_t *)(*cursor) = *value;
	*cursor += sizeof(uint8_t);
}

DQLITE_INLINE void uint16Encode(const uint16_t *value, void **cursor)
{
	*(uint16_t *)(*cursor) = byteFlip16(*value);
	*cursor += sizeof(uint16_t);
}

DQLITE_INLINE void uint32Encode(const uint32_t *value, void **cursor)
{
	*(uint32_t *)(*cursor) = byteFlip32(*value);
	*cursor += sizeof(uint32_t);
}

DQLITE_INLINE void uint64Encode(const uint64_t *value, void **cursor)
{
	*(uint64_t *)(*cursor) = byteFlip64(*value);
	*cursor += sizeof(uint64_t);
}

DQLITE_INLINE void int64Encode(const int64_t *value, void **cursor)
{
	*(int64_t *)(*cursor) = (int64_t)byteFlip64((uint64_t)*value);
	*cursor += sizeof(int64_t);
}

DQLITE_INLINE void floatEncode(const floatT *value, void **cursor)
{
	*(uint64_t *)(*cursor) = byteFlip64(*(uint64_t *)value);
	*cursor += sizeof(uint64_t);
}

DQLITE_INLINE void textEncode(const text_t *value, void **cursor)
{
	size_t len = bytePad64(strlen(*value) + 1);
	memset(*cursor, 0, len);
	strcpy(*cursor, *value);
	*cursor += len;
}

DQLITE_INLINE void blobEncode(const blobT *value, void **cursor)
{
	size_t len = bytePad64(value->len);
	uint64_t valueLen = value->len;
	uint64Encode(&valueLen, cursor);
	memcpy(*cursor, value->base, value->len);
	*cursor += len;
}

DQLITE_INLINE int uint8Decode(struct cursor *cursor, uint8_t *value)
{
	size_t n = sizeof(uint8_t);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = *(uint8_t *)cursor->p;
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int uint16Decode(struct cursor *cursor, uint16_t *value)
{
	size_t n = sizeof(uint16_t);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = byteFlip16(*(uint16_t *)cursor->p);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int uint32Decode(struct cursor *cursor, uint32_t *value)
{
	size_t n = sizeof(uint32_t);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = byteFlip32(*(uint32_t *)cursor->p);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int uint64Decode(struct cursor *cursor, uint64_t *value)
{
	size_t n = sizeof(uint64_t);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = byteFlip64(*(uint64_t *)cursor->p);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int int64Decode(struct cursor *cursor, int64_t *value)
{
	size_t n = sizeof(int64_t);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = (int64_t)byteFlip64((uint64_t) * (int64_t *)cursor->p);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int floatDecode(struct cursor *cursor, floatT *value)
{
	size_t n = sizeof(double);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	*(uint64_t *)value = byteFlip64(*(uint64_t *)cursor->p);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int textDecode(struct cursor *cursor, text_t *value)
{
	/* Find the terminating null byte of the next string, if any. */
	size_t len = strnlen(cursor->p, cursor->cap);
	size_t n;
	if (len == cursor->cap) {
		return DQLITE_PARSE;
	}
	*value = cursor->p;
	n = bytePad64(strlen(*value) + 1);
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

DQLITE_INLINE int blobDecode(struct cursor *cursor, blobT *value)
{
	uint64_t len;
	size_t n;
	int rv;
	rv = uint64Decode(cursor, &len);
	if (rv != 0) {
		return rv;
	}
	n = bytePad64(len);
	if (n > cursor->cap) {
		return DQLITE_PARSE;
	}
	value->base = (char *)cursor->p;
	value->len = len;
	cursor->p += n;
	cursor->cap -= n;
	return 0;
}

#endif /* LIB_SERIALIZE_H_ */
