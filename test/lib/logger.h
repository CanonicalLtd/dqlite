/**
 * Test logger.
 */

#ifndef TEST_LOGGER_H
#define TEST_LOGGER_H

#include "../../src/logger.h"

#include "munit.h"

void testLoggerSetup(const MunitParameter params[], struct logger *l);
void testLoggerTearDown(struct logger *l);

struct testLogger
{
	unsigned id;
	void *data;
};

void testLoggerEmit(void *data, int level, const char *fmt, va_list args);

#define FIXTURE_LOGGER struct logger logger;
#define SETUP_LOGGER testLoggerSetup(params, &f->logger);
#define TEAR_DOWN_LOGGER testLoggerTearDown(&f->logger);

#endif /* TEST_LOGGER_H */
