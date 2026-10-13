#ifndef LP_CONFIG_H
#define LP_CONFIG_H

#include "agent.h"

struct lp_config {
	int64_t config_version;
	int interval_s;
	int offline_after_s;
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

/* True when a change invalidates an in-progress measurement window. */
bool lp_config_changes_window(const struct lp_config *old, const struct lp_config *new);

#endif
