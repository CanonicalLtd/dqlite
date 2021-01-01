/**
 * Helper for test components supporting fault injection.
 */

#ifndef TEST_FAULT_H
#define TEST_FAULT_H

#include <stdbool.h>

/**
 * Information about a fault that should occurr in a component.
 */
struct testFault
{
    int countdown; /* Trigger the fault when this counter gets to zero. */
    int n;         /* Repeat the fault this many times. Default is -1. */
    bool enabled;   /* Enable fault triggering. */
};

/**
 * Initialize a fault.
 */
void testFaultInit(struct testFault *f);

/**
 * Advance the counters of the fault. Return true if the fault should be
 * triggered, false otherwise.
 */
bool testFaultTick(struct testFault *f);

/**
 * Configure the fault with the given values.
 */
void testFaultConfig(struct testFault *f, int delay, int repeat);

/**
 * Enable fault triggering.
 */
void testFaultEnable(struct testFault *f);

#endif /* TEST_FAULT_H */
