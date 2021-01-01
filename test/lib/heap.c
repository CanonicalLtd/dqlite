#include <sqlite3.h>

#include "fault.h"
#include "heap.h"

/* This structure is used to encapsulate the global state variables used by
 * malloc() fault simulation. */
struct mem_fault
{
	struct testFault fault;  /* Fault trigger */
	sqlite3_mem_methods m;   /* Actual malloc implementation */
};

/* We need to use a global variable here because after a sqlite3_mem_methods
 * instance has been installed using sqlite3_config(), and after
 * sqlite3_initialize() has been called, there's no way to retrieve it back with
 * sqlite3_config(). */
static struct mem_fault memFault;

/* A version of sqlite3_mem_methods.xMalloc() that includes fault simulation
 * logic.*/
static void *memFaultMalloc(int n)
{
	void *p = NULL;

	if (!testFaultTick(&memFault.fault)) {
		p = memFault.m.xMalloc(n);
	}

	return p;
}

/* A version of sqlite3_mem_methods.xRealloc() that includes fault simulation
 * logic. */
static void *memFaultRealloc(void *old, int n)
{
	void *p = NULL;

	if (!testFaultTick(&memFault.fault)) {
		p = memFault.m.xRealloc(old, n);
	}

	return p;
}

/* The following method calls are passed directly through to the underlying
 * malloc system:
 *
 *     xFree
 *     xSize
 *     xRoundup
 *     xInit
 *     xShutdown
 */
static void memFaultFree(void *p)
{
	memFault.m.xFree(p);
}

static int memFaultSize(void *p)
{
	return memFault.m.xSize(p);
}

static int memFaultRoundup(int n)
{
	return memFault.m.xRoundup(n);
}

static int memFaultInit(void *p)
{
	(void)p;
	return memFault.m.xInit(memFault.m.pAppData);
}

static void memFaultShutdown(void *p)
{
	(void)p;
	memFault.m.xShutdown(memFault.m.pAppData);
}

/* Wrap the given SQLite memory management instance with the faulty memory
 * management interface. By default no faults will be triggered. */
static void memWrap(sqlite3_mem_methods *m, sqlite3_mem_methods *wrap)
{
	testFaultInit(&memFault.fault);
	memFault.m = *m;

	wrap->xMalloc = memFaultMalloc;
	wrap->xFree = memFaultFree;
	wrap->xRealloc = memFaultRealloc;
	wrap->xSize = memFaultSize;
	wrap->xRoundup = memFaultRoundup;
	wrap->xInit = memFaultInit;
	wrap->xShutdown = memFaultShutdown;

	wrap->pAppData = &memFault;
}

/* Unwrap the given faulty memory management instance returning the original
 * one. */
static void memUnwrap(sqlite3_mem_methods *wrap, sqlite3_mem_methods *m)
{
	(void)wrap;

	*m = memFault.m;
}

/* Get the current number of outstanding malloc()'s without a matching free()
 * and the total number of used memory. */
static void memStats(int *mallocCount, int *memoryUsed)
{
	int rc;
	int watermark;

	rc = sqlite3_status(SQLITE_STATUS_MALLOC_COUNT, mallocCount, &watermark,
			    1);
	if (rc != SQLITE_OK) {
		munit_errorf("can't get malloc count: %s", sqlite3_errstr(rc));
	}

	rc = sqlite3_status(SQLITE_STATUS_MEMORY_USED, memoryUsed, &watermark,
			    1);
	if (rc != SQLITE_OK) {
		munit_errorf("can't get memory: %s\n:", sqlite3_errstr(rc));
	}
}

/* Ensure we're starting from a clean memory state with no allocations and
 * optionally inject malloc failures. */
void testHeapSetup(const MunitParameter params[], void *userData)
{
	int mallocCount;
	int memoryUsed;
	const char *faultDelay;
	const char *faultRepeat;
	sqlite3_mem_methods mem;
	sqlite3_mem_methods mem_fault;
	int rc;

	(void)params;
	(void)userData;

	/* Install the faulty malloc implementation */
	rc = sqlite3_config(SQLITE_CONFIG_GETMALLOC, &mem);
	if (rc != SQLITE_OK) {
		munit_errorf("can't get default mem: %s", sqlite3_errstr(rc));
	}

	memWrap(&mem, &mem_fault);

	rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &mem_fault);
	if (rc != SQLITE_OK) {
		munit_errorf("can't set faulty mem: %s", sqlite3_errstr(rc));
	}

	/* Check that memory is clean. */
	memStats(&mallocCount, &memoryUsed);
	if (mallocCount > 0 || memoryUsed > 0) {
		munit_errorf(
		    "setup memory:\n    bytes: %11d\n    allocations: %5d\n",
		    mallocCount, memoryUsed);
	}

	/* Optionally inject memory allocation failures. */
	faultDelay = munit_parameters_get(params, "mem-fault-delay");
	faultRepeat = munit_parameters_get(params, "mem-fault-repeat");

	munit_assert((faultDelay != NULL && faultRepeat != NULL) ||
		     (faultDelay == NULL && faultRepeat == NULL));

	if (faultDelay != NULL) {
		testHeapFaultConfig(atoi(faultDelay), atoi(faultRepeat));
	}
}

/* Ensure we're starting leaving a clean memory behind. */
void testHeapTearDown(void *data)
{
	sqlite3_mem_methods mem;
	sqlite3_mem_methods mem_fault;
	int rc;

	(void)data;

	int mallocCount;
	int memoryUsed;

	memStats(&mallocCount, &memoryUsed);
	if (mallocCount > 0 || memoryUsed > 0) {
		munit_errorf(
		    "teardown memory:\n    bytes: %11d\n    allocations: %5d\n",
		    memoryUsed, mallocCount);
	}

	/* Restore default memory management. */
	rc = sqlite3_config(SQLITE_CONFIG_GETMALLOC, &mem_fault);
	if (rc != SQLITE_OK) {
		munit_errorf("can't get faulty mem: %s", sqlite3_errstr(rc));
	}

	memUnwrap(&mem_fault, &mem);

	rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &mem);
	if (rc != SQLITE_OK) {
		munit_errorf("can't reset default mem: %s", sqlite3_errstr(rc));
	}
}

void testHeapFaultConfig(int delay, int repeat)
{
	testFaultConfig(&memFault.fault, delay, repeat);
}

void testHeapFaultEnable()
{
	testFaultEnable(&memFault.fault);
}
