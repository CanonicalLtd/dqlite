#ifndef LIB_BYTE_H_
#define LIB_BYTE_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__cplusplus)
#define DQLITE_INLINE inline
#else
#define DQLITE_INLINE static inline
#endif

/* Flip a 16-bit number to network byte order (little endian) */
DQLITE_INLINE uint16_t byteFlip16(uint16_t v)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __LITTLE_ENDIAN__)
	return v;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __BIG_ENDIAN__) && \
    defined(__GNUC__) && __GNUC__ >= 4 && __GNUC_MINOR__ >= 8
	return __builtin_bswap16(v);
#else
	union {
		uint16_t u;
		uint8_t v[2];
	} s;

	s.v[0] = (uint8_t)v;
	s.v[1] = (uint8_t)(v >> 8);

	return s.u;
#endif
}

/* Flip a 32-bit number to network byte order (little endian) */
DQLITE_INLINE uint32_t byteFlip32(uint32_t v)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __LITTLE_ENDIAN__)
	return v;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __BIG_ENDIAN__) && \
    defined(__GNUC__) && __GNUC__ >= 4 && __GNUC_MINOR__ >= 8
	return __builtin_bswap32(v);
#else
	union {
		uint32_t u;
		uint8_t v[4];
	} s;

	s.v[0] = (uint8_t)v;
	s.v[1] = (uint8_t)(v >> 8);
	s.v[2] = (uint8_t)(v >> 16);
	s.v[3] = (uint8_t)(v >> 24);

	return s.u;
#endif
}

/* Flip a 64-bit number to network byte order (little endian) */
DQLITE_INLINE uint64_t byteFlip64(uint64_t v)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __LITTLE_ENDIAN__)
	return v;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __BIG_ENDIAN__) && \
    defined(__GNUC__) && __GNUC__ >= 4 && __GNUC_MINOR__ >= 8
	return __builtin_bswap64(v);
#else
	union {
		uint64_t u;
		uint8_t v[8];
	} s;

	s.v[0] = (uint8_t)v;
	s.v[1] = (uint8_t)(v >> 8);
	s.v[2] = (uint8_t)(v >> 16);
	s.v[3] = (uint8_t)(v >> 24);
	s.v[4] = (uint8_t)(v >> 32);
	s.v[5] = (uint8_t)(v >> 40);
	s.v[6] = (uint8_t)(v >> 48);
	s.v[7] = (uint8_t)(v >> 56);

	return s.u;
#endif
}

/**
 * Add padding to size if it's not a multiple of 8. E.g. if 11 is passed, 16 is
 * returned.
 */
DQLITE_INLINE size_t bytePad64(size_t size)
{
	size_t rest = size % sizeof(uint64_t);
	if (rest != 0) {
		size += sizeof(uint64_t) - rest;
	}
	return size;
}

#endif /* LIB_BYTE_H_ */
