#ifndef DQLITE_TEST_HEAP_H
#define DQLITE_TEST_HEAP_H

#include "munit.h"

/* Munit parameter defining the delay of the faulty memory implementation. */
#define TEST_HEAP_FAULT_DELAY "mem-fault-delay"

/* Munit parameter defining the repeat of the faulty memory implementation. */
#define TEST_HEAP_FAULT_REPEAT "mem-fault-repeat"

void testHeapSetup(const MunitParameter params[], void *userData);
void testHeapTearDown(void *data);

/* Configure the faulty memory management implementation so malloc()-related
 * functions start returning NULL pointers after 'delay' calls, and keep failing
 * for 'repeat' consecutive times.
 *
 * Note that the faults won't automatically take place, an explicit call to
 * testMemFaultEnable() is needed. This allows configuration and actual
 * behavior to happen at different times (e.g. configure at test setup time and
 * enable at test case time). */
void testHeapFaultConfig(int delay, int repeat);

/* Enable the faulty behavior, which from this point on will honor the
 * parameters passed to testMemFaultConfig(). */
void testHeapFaultEnable(void);

#define SETUP_HEAP testHeapSetup(params, userData);
#define TEAR_DOWN_HEAP testHeapTearDown(data);

#endif /* DQLITE_TEST_HEAP_H */
