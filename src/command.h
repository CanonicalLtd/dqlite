/**
 * Encode and decode dqlite Raft FSM commands.
 */

#ifndef COMMAND_H_
#define COMMAND_H_

#include <raft.h>

#include "lib/serialize.h"

/* Command type codes */
enum { COMMAND_OPEN = 1, COMMAND_FRAMES, COMMAND_UNDO, COMMAND_CHECKPOINT };

/* Hold information about an array of WAL frames. */
struct frames
{
	uint32_t nPages;
	uint16_t pageSize;
	uint16_t __unused__;
	/* TODO: because the sqlite3 replication APIs are asymmetrics, the
	 * format differs between encode and decode. When encoding data is
	 * expected to be a sqlite3_wal_replication_frame* array, and when
	 * decoding it will be a pointer to raw memory which can be further
	 * decoded with the commandFramesPageNumbers() and
	 * commandFramesPages() helpers. */
	const void *data;
};

typedef struct frames frames_t;

/* Serialization definitions for a raft FSM command. */
#define COMMAND_DEFINE(LOWER, UPPER, _) \
	SERIALIZE_DEFINE_STRUCT(command##LOWER, COMMAND_##UPPER);

#define COMMAND_OPEN(X, ...) X(text, filename, ##__VA_ARGS__)
#define COMMAND_FRAMES(X, ...)               \
	X(text, filename, ##__VA_ARGS__)      \
	X(uint64, txId, ##__VA_ARGS__)        \
	X(uint32, truncate, ##__VA_ARGS__)    \
	X(uint8, isCommit, ##__VA_ARGS__)     \
	X(uint8, __unused1__, ##__VA_ARGS__)  \
	X(uint16, __unused2__, ##__VA_ARGS__) \
	X(frames, frames, ##__VA_ARGS__)
#define COMMAND_UNDO(X, ...) X(uint64, txId, ##__VA_ARGS__)
#define COMMAND_CHECKPOINT(X, ...) X(text, filename, ##__VA_ARGS__)

#define COMMAND_TYPES(X, ...)          \
	X(open, OPEN, __VA_ARGS__)     \
	X(frames, FRAMES, __VA_ARGS__) \
	X(undo, UNDO, __VA_ARGS__)     \
	X(checkpoint, CHECKPOINT, __VA_ARGS__)

COMMAND_TYPES(COMMAND_DEFINE);

int commandEncode(int type, const void *command, struct raft_buffer *buf);

int commandDecode(const struct raft_buffer *buf, int *type, void **command);

int commandFramesPageNumbers(const struct commandframes *c,
			     unsigned long *pageNumbers[]);

void commandFramesPages(const struct commandframes *c, void **pages);

#endif /* COMMAND_H_*/
