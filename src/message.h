#ifndef MESSAGE_H_
#define MESSAGE_H_

#include "lib/serialize.h"

/**
 * Metadata about an incoming or outgoing RPC message.
 */
#define MESSAGE(X, ...)          \
	X(uint32, words, ##__VA_ARGS__) \
	X(uint8, type, ##__VA_ARGS__)   \
	X(uint8, flags, ##__VA_ARGS__)  \
	X(uint16, extra, ##__VA_ARGS__)

SERIALIZE_DEFINE(message, MESSAGE);

#endif /* MESSAGE_HX */
