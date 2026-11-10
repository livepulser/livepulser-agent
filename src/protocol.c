#include "protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "platform.h"

const char *lp_metric_name(enum lp_metric m)
{
	return lp_metric_names[m];
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

bool lp_metric_present(const struct lp_metrics *m, int i)
{
	double limit = i < LP_M_NET_TX ? 100.0 : 1e12;

	return m->ok[i] && isfinite(m->value[i]) && m->value[i] >= 0 && m->value[i] <= limit;
}

/*
 * Samples are written as [end_offset_ms, duration_ms, metrics...] relative to the
 * first sample's start. The process never calls setlocale(), so printf uses '.'.
 */
int lp_report_write(const struct lp_report *r, char *buf, size_t cap, size_t *len)
{
	size_t pos = 0;
	int64_t start;
	bool ok;

	if (r->count == 0 || r->count > LP_MAX_SAMPLES || r->sequence < 1 || r->sequence > LP_MAX_SEQUENCE ||
	    r->config_version < 1 || strlen(r->arch) > 32)
		return -1;
	start = r->samples[0].end_ms - r->samples[0].duration_ms;
	ok = append(buf, cap, &pos,
		    "{\"schema_version\":2,\"boot_id\":\"%s\",\"sequence\":%lld,\"config_version\":%lld,\"trigger\":\"%s\","
		    "\"agent\":{\"name\":\"" LP_AGENT_NAME "\",\"version\":\"" LP_VERSION "\",\"os\":\"%s\",\"arch\":\"%s\"},"
		    "\"start_ms\":%lld,\"samples\":[",
		    r->boot_id, (long long)r->sequence, (long long)r->config_version, r->trigger, lp_os_name(), r->arch,
		    (long long)start);
	for (size_t n = 0; ok && n < r->count; n++) {
		const struct lp_sample *smp = &r->samples[n];
		if (smp->duration_ms < 1000 || smp->duration_ms > 3600000 || smp->end_ms - start < smp->duration_ms)
			return -1;
		ok = append(buf, cap, &pos, "%s[%lld,%lld", n ? "," : "", (long long)(smp->end_ms - start), (long long)smp->duration_ms);
		for (int i = 0; ok && i < LP_M_COUNT; i++) {
			if (!lp_metric_present(&smp->metrics, i))
				ok = append(buf, cap, &pos, ",null");
			else if (i < LP_M_NET_TX)
				ok = append(buf, cap, &pos, ",%.2f", smp->metrics.value[i]);
			else
				ok = append(buf, cap, &pos, ",%.0f", smp->metrics.value[i]);
		}
		ok = ok && append(buf, cap, &pos, "]");
	}
	ok = ok && append(buf, cap, &pos, "]}");
	if (!ok || pos > LP_MAX_REPORT_BYTES)
		return -1;
	*len = pos;
	return 0;
}
