#ifndef LOGGER_H_
#define LOGGER_H_

#include <raft.h>

#include "../include/dqlite.h"

/* Log levels */
enum { DQLITE_DEBUG = 0, DQLITE_INFO, DQLITE_WARN, DQLITE_LOG_ERROR };

/* Function to emit log messages. */
typedef void (*dqliteEmit)(void *data,
			   int level,
			   const char *fmt,
			   va_list args);

struct logger
{
	void *data;
	dqliteEmit emit;
};

/* Default implementation of dqliteEmit, using stderr. */
void loggerDefaultEmit(void *data, int level, const char *fmt, va_list args);

/* Emit a log message with a certain level. */
/* #define debugf(L, FORMAT, ...) \ */
/* 	loggerEmit(L, DQLITE_DEBUG, FORMAT, ##__VA_ARGS__) */
#define debugf(C, FORMAT, ...)                                             \
	C->gateway.raft->io->emit(C->gateway.raft->io, RAFT_DEBUG, FORMAT, \
				  ##__VA_ARGS__)

#endif /* LOGGER_H_ */
