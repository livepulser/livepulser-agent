#ifndef LP_COLLECTOR_H
#define LP_COLLECTOR_H

#include "agent.h"
#include "config.h"
#include "procfs.h"
#include "protocol.h"

struct lp_counters {
	struct lp_clock clock;
	struct lp_cpu cpu;
	bool net_valid;
	char ifname[LP_IFNAME_SIZE];
	unsigned ifindex;
	uint64_t rx;
	uint64_t tx;
};

struct lp_collector {
	char proc_root[256];
	char ifname[LP_IFNAME_SIZE];
	int route_status;
	int64_t route_checked_ms;
};

void lp_collector_init(struct lp_collector *c);
void lp_clock_now(struct lp_clock *t);
int64_t lp_mono_ms(void);

/* Returns true when the selected interface changed. */
bool lp_collector_select(struct lp_collector *c, const struct lp_config *cfg, int64_t now_ms, bool force);
void lp_collect_counters(const struct lp_collector *c, struct lp_counters *out);
void lp_collect_metrics(const struct lp_collector *c, const struct lp_config *cfg, const struct lp_counters *start,
			const struct lp_counters *end, int64_t duration_ms, struct lp_metrics *m);

#endif
