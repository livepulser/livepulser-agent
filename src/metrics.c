#include "metrics.h"

int lp_cpu_finish(struct lp_cpu *c)
{
	c->total = 0;
	for (int i = 0; i < LP_CPU_FIELDS; i++) {
		if (c->total > UINT64_MAX - c->field[i])
			return -1;
		c->total += c->field[i];
	}
	c->valid = c->idle <= c->total;
	return c->valid ? 0 : -1;
}

int lp_cpu_percent(const struct lp_cpu *a, const struct lp_cpu *b, double *out)
{
	uint64_t dt, di;

	if (!a->valid || !b->valid || a->ncpu != b->ncpu)
		return -1;
	if (b->total < a->total || b->idle < a->idle)
		return -1;
	for (int i = 0; i < LP_CPU_FIELDS; i++) {
		if (!(b->may_decrease & (1u << i)) && b->field[i] < a->field[i])
			return -1;
	}
	dt = b->total - a->total;
	di = b->idle - a->idle;
	if (dt == 0 || di > dt)
		return -1;
	*out = 100.0 * (double)(dt - di) / (double)dt;
	return 0;
}

int lp_rate(uint64_t start, uint64_t end, int64_t elapsed_ms, double *out)
{
	double rate;

	if (end < start || elapsed_ms <= 0)
		return -1;
	rate = (double)(end - start) * 1000.0 / (double)elapsed_ms;
	if (rate > 1e12)
		return -1;
	*out = rate;
	return 0;
}

int lp_used_percent(uint64_t total, uint64_t free, double *out)
{
	if (total == 0 || free > total)
		return -1;
	*out = 100.0 * (double)(total - free) / (double)total;
	return 0;
}
