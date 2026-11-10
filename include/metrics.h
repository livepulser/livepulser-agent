#ifndef LP_METRICS_H
#define LP_METRICS_H

#include "agent.h"

#define LP_CPU_FIELDS 8

/* Cumulative CPU time counters; units are irrelevant because only ratios are used. */
struct lp_cpu {
	bool valid;
	int ncpu;
	unsigned may_decrease;
	uint64_t field[LP_CPU_FIELDS];
	uint64_t total;
	uint64_t idle;
};

enum lp_route_result {
	LP_ROUTE_FOUND,
	LP_ROUTE_NONE,
	LP_ROUTE_AMBIGUOUS,
};

int lp_cpu_finish(struct lp_cpu *c);
int lp_cpu_percent(const struct lp_cpu *a, const struct lp_cpu *b, double *out);
int lp_rate(uint64_t start, uint64_t end, int64_t elapsed_ms, double *out);
int lp_used_percent(uint64_t total, uint64_t free, double *out);

#endif
