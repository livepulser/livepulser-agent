#ifndef LP_PROTOCOL_H
#define LP_PROTOCOL_H

#include "agent.h"

enum lp_metric {
	LP_M_CPU,
	LP_M_MEMORY,
	LP_M_DISK,
	LP_M_SWAP,
	LP_M_NET_TX,
	LP_M_NET_RX,
	LP_M_COUNT,
};

struct lp_metrics {
	bool ok[LP_M_COUNT];
	double value[LP_M_COUNT];
};

struct lp_sample {
	int64_t end_ms;
	int64_t duration_ms;
	struct lp_metrics metrics;
};

struct lp_report {
	char boot_id[LP_UUID_LEN + 1];
	int64_t sequence;
	int64_t config_version;
	const char *trigger;
	const char *arch;
	const struct lp_sample *samples;
	size_t count;
};

bool lp_metric_present(const struct lp_metrics *m, int i);
const char *lp_metric_name(enum lp_metric m);
int lp_report_write(const struct lp_report *r, char *buf, size_t cap, size_t *len);

#endif
