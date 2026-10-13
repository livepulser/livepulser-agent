#ifndef LP_COLLECTOR_H
#define LP_COLLECTOR_H

#include "agent.h"
#include "config.h"
#include "metrics.h"
#include "protocol.h"

struct lp_counters {
	struct lp_clock clock;
	struct lp_cpu cpu;
	bool net_valid;
	char ifname[LP_IFNAME_SIZE];
	uint64_t if_id;
	uint64_t rx;
	uint64_t tx;
};

struct lp_collector {
	char ifname[LP_IFNAME_SIZE];
	int route_status;
	int64_t route_checked_ms;
};

int lp_collector_init(struct lp_collector *c);
void lp_collector_cleanup(void);

/* Returns true when the selected interface changed. */
bool lp_collector_select(struct lp_collector *c, const struct lp_config *cfg, int64_t now_ms, bool force);
void lp_collect_counters(const struct lp_collector *c, struct lp_counters *out);
void lp_collect_metrics(const struct lp_config *cfg, const struct lp_counters *start, const struct lp_counters *end,
			int64_t duration_ms, struct lp_metrics *m);

/* Implemented once per operating system. Each returns 0 on success, -1 when unavailable. */
int lp_os_init(void);
void lp_os_cleanup(void);
int lp_os_cpu(struct lp_cpu *out);
int lp_os_memory(double *out);
int lp_os_swap(double *out);
int lp_os_disk(const char *path, double *out);
int lp_os_net(const char *ifname, uint64_t *id, uint64_t *rx, uint64_t *tx);
enum lp_route_result lp_os_default_interface(char name[LP_IFNAME_SIZE]);

#endif
