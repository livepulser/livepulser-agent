#include "rules.h"

#include <string.h>

void lp_rules_reset(struct lp_rules *r)
{
	bool alerting = r->alerting;
	bool cause[LP_M_COUNT];

	memcpy(cause, r->cause, sizeof cause);
	memset(r, 0, sizeof *r);
	r->alerting = alerting;
	memcpy(r->cause, cause, sizeof cause);
}

static double recovery_level(const struct lp_config *c, int i)
{
	if (i >= LP_M_NET_TX)
		return c->threshold[i] * (1 - c->recovery_margin / 100);
	return c->threshold[i] - c->recovery_margin;
}

enum lp_rule_event lp_rules_feed(struct lp_rules *r, const struct lp_config *c, const struct lp_sample *s)
{
	int64_t start = s->end_ms - s->duration_ms;
	bool breached = false, recovered = true;

	if (s->end_ms <= r->last_end)
		return LP_RULE_NONE;
	if (r->last_end > 0 && start - r->last_end > 2 * s->duration_ms)
		lp_rules_reset(r);
	r->last_end = s->end_ms;

	for (int i = 0; i < LP_M_COUNT; i++) {
		if (!c->has_threshold[i] || !lp_metric_present(&s->metrics, i)) {
			r->above_since[i] = r->below_since[i] = 0;
			continue;
		}
		double v = s->metrics.value[i];
		if (v >= c->threshold[i]) {
			if (!r->above_since[i])
				r->above_since[i] = start;
		} else {
			r->above_since[i] = 0;
		}
		if (v <= recovery_level(c, i)) {
			if (!r->below_since[i])
				r->below_since[i] = start;
		} else {
			r->below_since[i] = 0;
		}
		if (r->above_since[i] && s->end_ms - r->above_since[i] >= (int64_t)c->breach_s * 1000 && !r->cause[i]) {
			r->cause[i] = true;
			breached = true;
		}
	}
	if (breached) {
		r->alerting = true;
		return LP_RULE_BREACH;
	}
	if (!r->alerting)
		return LP_RULE_NONE;
	for (int i = 0; i < LP_M_COUNT; i++) {
		if (r->cause[i] && c->has_threshold[i] &&
		    !(r->below_since[i] && s->end_ms - r->below_since[i] >= (int64_t)c->recovery_s * 1000))
			recovered = false;
	}
	if (!recovered)
		return LP_RULE_NONE;
	r->alerting = false;
	memset(r->cause, 0, sizeof r->cause);
	return LP_RULE_RECOVERY;
}
