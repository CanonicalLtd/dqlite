#include <stdarg.h>
#include <stdio.h>

#include "../../include/dqlite.h"

#include "logger.h"
#include "munit.h"

void test_loggerEmit(void *data, int level, const char *format, va_list args)
{
	struct test_logger *t = data;
	char buf[1024];
	const char *levelName;
	int i;

	(void)data;

	switch (level) {
		case DQLITE_DEBUG:
			levelName = "DEBUG";
			break;
		case DQLITE_INFO:
			levelName = "INFO ";
			break;
		case DQLITE_WARN:
			levelName = "WARN ";
			break;
		case DQLITE_LOG_ERROR:
			levelName = "ERROR";
			break;
	};

	buf[0] = 0;

	sprintf(buf + strlen(buf), "%2d -> [%s] ", t->id, levelName);

	vsnprintf(buf + strlen(buf), 1024 - strlen(buf), format, args);
	munit_log(MUNIT_LOG_INFO, buf);
	return;

	snprintf(buf + strlen(buf), 1024 - strlen(buf), " ");

	for (i = strlen(buf); i < 85; i++) {
		buf[i] = ' ';
	}

	munit_log(MUNIT_LOG_INFO, buf);
}

void test_logger_setup(const MunitParameter params[], struct logger *l)
{
	struct test_logger *t;

	(void)params;

	t = munit_malloc(sizeof *t);
	t->data = NULL;

	l->data = t;
	l->emit = test_loggerEmit;
}

void test_logger_tear_down(struct logger *l)
{
	free(l->data);
}
