#include "config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"

const char *const lp_metric_names[LP_METRIC_COUNT] = {
	"cpu_percent",
	"memory_percent",
	"disk_percent",
	"swap_percent",
	"network_tx_bytes_per_second",
	"network_rx_bytes_per_second",
};

static bool get_number(const cJSON *obj, const char *key, double min, double max, double *out)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

	if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < min || item->valuedouble > max)
		return false;
	*out = item->valuedouble;
	return true;
}

static bool get_integer(const cJSON *obj, const char *key, int64_t min, int64_t max, int64_t *out)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	double v;

	if (!cJSON_IsNumber(item))
		return false;
	v = item->valuedouble;
	if (!isfinite(v) || v != floor(v) || v < (double)min || v > (double)max)
		return false;
	*out = (int64_t)v;
	return true;
}

enum lp_config_result lp_config_parse(const char *json, size_t len, struct lp_config *out, char *err, size_t errlen)
{
	enum lp_config_result result = LP_CONFIG_INVALID;
	const cJSON *settings, *active, *thresholds, *rules, *item;
	struct lp_config c = {0};
	int64_t v;
	cJSON *root;

	if (len == 0 || len > LP_MAX_CONFIG_BYTES) {
		snprintf(err, errlen, "configuration body size %zu is out of range", len);
		return LP_CONFIG_INVALID;
	}
	root = cJSON_ParseWithLength(json, len);
	if (!cJSON_IsObject(root)) {
		snprintf(err, errlen, "configuration is not a JSON object");
		goto done;
	}
	if (!get_integer(root, "schema_version", 1, LP_MAX_SEQUENCE, &v)) {
		snprintf(err, errlen, "missing or invalid schema_version");
		goto done;
	}
	if (v != 2) {
		snprintf(err, errlen, "unsupported configuration schema_version %lld", (long long)v);
		result = LP_CONFIG_UNSUPPORTED;
		goto done;
	}
	if (!get_integer(root, "config_version", 1, LP_MAX_SEQUENCE, &c.config_version)) {
		snprintf(err, errlen, "missing or invalid config_version");
		goto done;
	}
	if (!get_integer(root, "report_interval_seconds", LP_MIN_INTERVAL_S, LP_MAX_INTERVAL_S, &v)) {
		snprintf(err, errlen, "missing or invalid report_interval_seconds");
		goto done;
	}
	c.interval_s = (int)v;
	if (!get_integer(root, "sample_seconds", 1, 60, &v) || v > c.interval_s) {
		snprintf(err, errlen, "missing or invalid sample_seconds");
		goto done;
	}
	c.sample_s = (int)v;
	c.offline_after_s = c.interval_s * 3 + 30;
	if (cJSON_HasObjectItem(root, "offline_after_seconds")) {
		if (!get_integer(root, "offline_after_seconds", 1, 4 * LP_MAX_INTERVAL_S, &v)) {
			snprintf(err, errlen, "invalid offline_after_seconds");
			goto done;
		}
		c.offline_after_s = (int)v;
	}
	active = cJSON_GetObjectItemCaseSensitive(root, "active");
	if (!cJSON_IsBool(active)) {
		snprintf(err, errlen, "missing or invalid active flag");
		goto done;
	}
	c.active = cJSON_IsTrue(active);
	thresholds = cJSON_GetObjectItemCaseSensitive(root, "thresholds");
	settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
	rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
	if (!cJSON_IsObject(settings) || !cJSON_IsObject(thresholds) || !cJSON_IsObject(rules)) {
		snprintf(err, errlen, "missing settings, thresholds or rules object");
		goto done;
	}
	for (int i = 0; i < LP_METRIC_COUNT; i++) {
		if (!cJSON_HasObjectItem(thresholds, lp_metric_names[i]))
			continue;
		if (!get_number(thresholds, lp_metric_names[i], 0, 1e12, &c.threshold[i])) {
			snprintf(err, errlen, "invalid threshold for %s", lp_metric_names[i]);
			goto done;
		}
		c.has_threshold[i] = true;
	}
	if (!get_integer(rules, "breach_seconds", 1, 3600, &v)) {
		snprintf(err, errlen, "invalid rules.breach_seconds");
		goto done;
	}
	c.breach_s = (int)v;
	if (!get_integer(rules, "recovery_seconds", 1, 3600, &v)) {
		snprintf(err, errlen, "invalid rules.recovery_seconds");
		goto done;
	}
	c.recovery_s = (int)v;
	if (!get_number(rules, "recovery_margin", 0, 100, &c.recovery_margin)) {
		snprintf(err, errlen, "invalid rules.recovery_margin");
		goto done;
	}

	snprintf(c.disk_path, sizeof c.disk_path, "/");
	c.disk_path_valid = true;
	item = cJSON_GetObjectItemCaseSensitive(settings, "disk_path");
	if (item) {
		if (!cJSON_IsString(item)) {
			snprintf(err, errlen, "settings.disk_path is not a string");
			goto done;
		}
		if (item->valuestring[0] != '\0') {
			size_t n = strlen(item->valuestring);
			c.disk_path_valid = n < sizeof c.disk_path && lp_valid_disk_path(item->valuestring);
			if (c.disk_path_valid)
				memcpy(c.disk_path, item->valuestring, n + 1);
		}
	}

	c.interface_valid = true;
	item = cJSON_GetObjectItemCaseSensitive(settings, "network_interface");
	if (item) {
		if (!cJSON_IsString(item)) {
			snprintf(err, errlen, "settings.network_interface is not a string");
			goto done;
		}
		if (item->valuestring[0] != '\0') {
			c.interface_explicit = true;
			c.interface_valid = lp_valid_ifname(item->valuestring);
			if (c.interface_valid)
				snprintf(c.ifname, sizeof c.ifname, "%s", item->valuestring);
		}
	}

	*out = c;
	result = LP_CONFIG_OK;
done:
	cJSON_Delete(root);
	return result;
}

bool lp_config_changes_window(const struct lp_config *old, const struct lp_config *new)
{
	return old->sample_s != new->sample_s || old->active != new->active ||
	       old->disk_path_valid != new->disk_path_valid || strcmp(old->disk_path, new->disk_path) != 0 ||
	       old->interface_explicit != new->interface_explicit || old->interface_valid != new->interface_valid ||
	       strcmp(old->ifname, new->ifname) != 0;
}
