/* Utilities around SQLite file formats.
 *
 * See https://sqlite.org/fileformat.html. */

#ifndef FORMAT_H_
#define FORMAT_H_

#include <stdbool.h>
#include <stdint.h>

/* Minumum and maximum page size. */
#define FORMAT_PAGE_SIZE_MIN 512
#define FORMAT_PAGE_SIZE_MAX 65536

/* Database header size. */
#define FORMAT_DB_HDR_SIZE 100

/* Write ahead log header size. */
#define FORMAT_WAL_HDR_SIZE 32

/* Write ahead log frame header size. */
#define FORMAT_WAL_FRAME_HDR_SIZE 24

/* Number of reader marks in the wal index header. */
#define FORMAT_WAL_NREADER 5

/* Given the page size, calculate the size of a full WAL frame (frame header
 * plus page data). */
#define formatWalCalcFrameSize(PAGE_SIZE) \
	(FORMAT_WAL_FRAME_HDR_SIZE + PAGE_SIZE)

/* Given the page size and the WAL file size, calculate the number of frames it
 * has. */
#define formatWalCalcFramesNumber(PAGE_SIZE, SIZE) \
	((SIZE - FORMAT_WAL_HDR_SIZE) / formatWalCalcFrameSize(PAGE_SIZE))

/* Given the page size, calculate the WAL page number of the frame starting at
 * the given offset. */
#define formatWalCalcFrameIndex(PAGE_SIZE, OFFSET) \
	(formatWalCalcFramesNumber(PAGE_SIZE, OFFSET) + 1)

/* Restart the header of a WAL file after a checkpoint. */
void formatWalRestartHeader(uint8_t *header);

#endif /* FORMAT_H */
