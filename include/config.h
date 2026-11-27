#ifndef LP_CONFIG_H
#define LP_CONFIG_H

#include "agent.h"

#define LP_METRIC_COUNT 6

struct lp_config {
	int64_t config_version;
	int interval_s;
	int sample_s;
	int offline_after_s;
	bool has_threshold[LP_METRIC_COUNT];
	double threshold[LP_METRIC_COUNT];
	int breach_s;
	int recovery_s;
	double recovery_margin;
	bool active;
	bool disk_path_valid;
	char disk_path[LP_DISK_PATH_SIZE];
	bool interface_explicit;
	bool interface_valid;
	char ifname[LP_IFNAME_SIZE];
};

enum lp_config_result {
	LP_CONFIG_OK,
	LP_CONFIG_INVALID,
	LP_CONFIG_UNSUPPORTED,
};

enum lp_config_result lp_config_parse(const char *json, size_t len, struct lp_config *out, char *err, size_t errlen);

extern const char *const lp_metric_names[LP_METRIC_COUNT];

/* True when a change invalidates an in-progress measurement window. */
bool lp_config_changes_window(const struct lp_config *old, const struct lp_config *new);

#endif
