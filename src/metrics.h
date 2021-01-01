/******************************************************************************
 *
 * Collect various performance metrics.
 *
 *****************************************************************************/

#ifndef DQLITE_METRICS_H
#define DQLITE_METRICS_H

#include <stdint.h>

struct dqliteMetrics
{
	uint64_t requests; /* Total number of requests served. */
	uint64_t duration; /* Total time spent to server requests. */
};

void dqliteMetricsInit(struct dqliteMetrics *m);

#endif /* DQLITE_METRICS_H */
