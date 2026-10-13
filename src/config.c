#include "config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "procfs.h"

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
	const cJSON *settings, *active, *thresholds, *item;
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
	if (v != 1) {
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
	if (!cJSON_IsObject(settings) || !cJSON_IsObject(thresholds)) {
		snprintf(err, errlen, "missing settings or thresholds object");
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
			c.disk_path_valid = item->valuestring[0] == '/' && n < sizeof c.disk_path;
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
				snprintf(c.interface, sizeof c.interface, "%s", item->valuestring);
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
	return old->interval_s != new->interval_s || old->active != new->active ||
	       old->disk_path_valid != new->disk_path_valid || strcmp(old->disk_path, new->disk_path) != 0 ||
	       old->interface_explicit != new->interface_explicit || old->interface_valid != new->interface_valid ||
	       strcmp(old->interface, new->interface) != 0;
}
