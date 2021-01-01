#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "./lib/assert.h"

#include "format.h"

/* WAL magic value. Either this value, or the same value with the least
 * significant bit also set (FORMAT_WAL_MAGIC | 0x00000001) is stored in 32-bit
 * big-endian format in the first 4 bytes of a WAL file.
 *
 * If the LSB is set, then the checksums for each frame within the WAL file are
 * calculated by treating all data as an array of 32-bit big-endian
 * words. Otherwise, they are calculated by interpreting all data as 32-bit
 * little-endian words. */
#define FORMAT_WAL_MAGIC 0x377f0682

#define FORMAT_WAL_MAX_VERSION 3007000

static void formatGet32(const uint8_t buf[4], uint32_t *v)
{
	*v = 0;
	*v += (uint32_t)(buf[0] << 24);
	*v += (uint32_t)(buf[1] << 16);
	*v += (uint32_t)(buf[2] << 8);
	*v += (uint32_t)(buf[3]);
}

/* Encode a 32-bit number to big endian format */
static void formatPut32(uint32_t v, uint8_t *buf)
{
	buf[0] = (uint8_t)(v >> 24);
	buf[1] = (uint8_t)(v >> 16);
	buf[2] = (uint8_t)(v >> 8);
	buf[3] = (uint8_t)v;
}

/*
 * Generate or extend an 8 byte checksum based on the data in array data[] and
 * the initial values of in[0] and in[1] (or initial values of 0 and 0 if
 * in==NULL).
 *
 * The checksum is written back into out[] before returning.
 *
 * n must be a positive multiple of 8. */
static void formatWalChecksumBytes(
    bool native,   /* True for native byte-order, false for non-native */
    uint8_t *data, /* Content to be checksummed */
    unsigned n,    /* Bytes of content in a[].  Must be a multiple of 8. */
    const uint32_t in[2], /* Initial checksum value input */
    uint32_t out[2]       /* OUT: Final checksum value output */
)
{
	uint32_t s1, s2;
	uint32_t *cur = (uint32_t *)data;
	uint32_t *end = (uint32_t *)&data[n];

	if (in) {
		s1 = in[0];
		s2 = in[1];
	} else {
		s1 = s2 = 0;
	}

	assert(n >= 8);
	assert((n & 0x00000007) == 0);
	assert(n <= 65536);

	if (native) {
		do {
			s1 += *cur++ + s2;
			s2 += *cur++ + s1;
		} while (cur < end);
	} else {
		do {
			uint32_t d;
			formatPut32(cur[0], (uint8_t *)&d);
			s1 += d + s2;
			formatPut32(cur[1], (uint8_t *)&d);
			s2 += d + s1;
			cur += 2;
		} while (cur < end);
	}

	out[0] = s1;
	out[1] = s2;
}

void formatWalRestartHeader(uint8_t *header) {
	uint32_t checksum[2] = {0, 0};
	uint32_t checkpoint;
	uint32_t salt1;

	/* Increase the checkpoint sequence. */
	formatGet32(&header[12], &checkpoint);
	checkpoint++;
	formatPut32(checkpoint, &header[12]);

	/* Increase salt1. */
	formatGet32(&header[16], &salt1);
	salt1++;
	formatPut32(salt1, &header[16]);

	/* Generate a random salt2. */
	sqlite3_randomness(4, &header[20]);

	/* Update the checksum. */
	formatWalChecksumBytes(true, header, 24, checksum, checksum);
	formatPut32(checksum[0], header + 24);
	formatPut32(checksum[1], header + 28);
}
