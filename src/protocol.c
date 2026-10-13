#include "protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "platform.h"

static const char *const metric_names[LP_M_COUNT] = {
	"cpu_percent",
	"memory_percent",
	"disk_percent",
	"swap_percent",
	"network_tx_bytes_per_second",
	"network_rx_bytes_per_second",
};

const char *lp_metric_name(enum lp_metric m)
{
	return metric_names[m];
}

int lp_format_rfc3339(int64_t unix_ms, char *out, size_t cap)
{
	struct tm tm;

	if (!lp_gmtime(unix_ms / 1000, &tm) || strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
		return -1;
	return 0;
}

static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static bool append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (*pos >= cap)
		return false;
	va_start(ap, fmt);
	n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= cap - *pos)
		return false;
	*pos += (size_t)n;
	return true;
}

static bool present(const struct lp_metrics *m, int i)
{
	double limit = i < LP_M_NET_TX ? 100.0 : 1e12;

	return m->ok[i] && isfinite(m->value[i]) && m->value[i] >= 0 && m->value[i] <= limit;
}

/* The process never calls setlocale(), so printf uses the C locale's '.' separator. */
int lp_report_write(const struct lp_report *r, char *buf, size_t cap, size_t *len)
{
	char observed[32];
	size_t pos = 0;
	bool first = true, ok;

	if (r->sequence < 1 || r->sequence > LP_MAX_SEQUENCE || r->config_version < 1 ||
	    r->duration_ms < 1000 || r->duration_ms > 3600000 || strlen(r->arch) > 32 ||
	    lp_format_rfc3339(r->observed_unix_ms, observed, sizeof observed))
		return -1;

	ok = append(buf, cap, &pos,
		    "{\"schema_version\":1,\"boot_id\":\"%s\",\"sequence\":%lld,\"config_version\":%lld,"
		    "\"observed_at\":\"%s\",\"sample_duration_ms\":%lld,"
		    "\"agent\":{\"name\":\"" LP_AGENT_NAME "\",\"version\":\"" LP_VERSION "\",\"os\":\"%s\",\"arch\":\"%s\"},"
		    "\"metrics\":{",
		    r->boot_id, (long long)r->sequence, (long long)r->config_version, observed,
		    (long long)r->duration_ms, lp_os_name(), r->arch);

	for (int i = 0; ok && i < LP_M_COUNT; i++) {
		const char *sep = i ? "," : "";

		if (!present(&r->metrics, i))
			ok = append(buf, cap, &pos, "%s\"%s\":null", sep, metric_names[i]);
		else if (i < LP_M_NET_TX)
			ok = append(buf, cap, &pos, "%s\"%s\":%.2f", sep, metric_names[i], r->metrics.value[i]);
		else
			ok = append(buf, cap, &pos, "%s\"%s\":%.0f", sep, metric_names[i], r->metrics.value[i]);
	}
	ok = ok && append(buf, cap, &pos, "},\"unavailable_metrics\":[");
	for (int i = 0; ok && i < LP_M_COUNT; i++) {
		if (present(&r->metrics, i))
			continue;
		ok = append(buf, cap, &pos, "%s\"%s\"", first ? "" : ",", metric_names[i]);
		first = false;
	}
	ok = ok && append(buf, cap, &pos, "]}");
	if (!ok || pos > LP_MAX_REPORT_BYTES)
		return -1;
	*len = pos;
	return 0;
}
