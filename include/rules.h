#ifndef LP_RULES_H
#define LP_RULES_H

#include "config.h"
#include "protocol.h"

/*
 * Local mirror of the server's alert rule: a metric breaches after staying at or
 * above its threshold for breach_s, and an alert recovers once every causing
 * metric stays at or below its recovery level for recovery_s. The server
 * re-evaluates the same samples; the agent only uses this to upload promptly.
 */
struct lp_rules {
	int64_t above_since[LP_M_COUNT];
	int64_t below_since[LP_M_COUNT];
	bool cause[LP_M_COUNT];
	bool alerting;
	int64_t last_end;
};

enum lp_rule_event {
	LP_RULE_NONE,
	LP_RULE_BREACH,
	LP_RULE_RECOVERY,
};

void lp_rules_reset(struct lp_rules *r);
enum lp_rule_event lp_rules_feed(struct lp_rules *r, const struct lp_config *c, const struct lp_sample *s);

#endif
