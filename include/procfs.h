#ifndef LP_PROCFS_H
#define LP_PROCFS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "agent.h"

struct lp_cpu {
	bool valid;
	int ncpu;
	uint64_t field[8];
	uint64_t total;
	uint64_t idle;
};

struct lp_meminfo {
	unsigned have;
	uint64_t mem_total;
	uint64_t mem_available;
	uint64_t swap_total;
	uint64_t swap_free;
};

enum lp_route_result {
	LP_ROUTE_FOUND,
	LP_ROUTE_NONE,
	LP_ROUTE_AMBIGUOUS,
};

int lp_parse_stat(FILE *f, struct lp_cpu *out);
int lp_cpu_percent(const struct lp_cpu *a, const struct lp_cpu *b, double *out);

int lp_parse_meminfo(FILE *f, struct lp_meminfo *out);
int lp_memory_percent(const struct lp_meminfo *m, double *out);
int lp_swap_percent(const struct lp_meminfo *m, double *out);

int lp_parse_netdev(FILE *f, const char *ifname, uint64_t *rx, uint64_t *tx);
int lp_rate(uint64_t start, uint64_t end, int64_t elapsed_ms, double *out);

enum lp_route_result lp_route_v4(FILE *f, char out[LP_IFNAME_SIZE]);
enum lp_route_result lp_route_v6(FILE *f, char out[LP_IFNAME_SIZE]);

int lp_disk_percent(uint64_t blocks, uint64_t bfree, double *out);
bool lp_valid_ifname(const char *name);

#endif
