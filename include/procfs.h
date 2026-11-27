#ifndef LP_PROCFS_H
#define LP_PROCFS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "metrics.h"

struct lp_meminfo {
	unsigned have;
	uint64_t mem_total;
	uint64_t mem_available;
	uint64_t swap_total;
	uint64_t swap_free;
};

int lp_parse_stat(FILE *f, struct lp_cpu *out);

int lp_parse_meminfo(FILE *f, struct lp_meminfo *out);
int lp_memory_percent(const struct lp_meminfo *m, double *out);
int lp_swap_percent(const struct lp_meminfo *m, double *out);

int lp_parse_netdev(FILE *f, const char *ifname, uint64_t *rx, uint64_t *tx);

enum lp_route_result lp_route_v4(FILE *f, char out[LP_IFNAME_SIZE]);
enum lp_route_result lp_route_v6(FILE *f, char out[LP_IFNAME_SIZE]);

#endif
