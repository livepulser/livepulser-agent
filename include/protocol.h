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

struct lp_report {
	char boot_id[LP_UUID_LEN + 1];
	int64_t sequence;
	int64_t config_version;
	int64_t observed_unix_ms;
	int64_t duration_ms;
	const char *arch;
	struct lp_metrics metrics;
};

const char *lp_metric_name(enum lp_metric m);
const char *lp_arch(void);
int lp_format_rfc3339(int64_t unix_ms, char *out, size_t cap);
int lp_report_write(const struct lp_report *r, char *buf, size_t cap, size_t *len);

#endif
