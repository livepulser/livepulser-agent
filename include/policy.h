#ifndef LP_POLICY_H
#define LP_POLICY_H

#include "agent.h"

/* Client-side limit: at most two requests of one kind per rolling minute. */
struct lp_guard {
	int64_t at[2];
};

enum lp_window_status {
	LP_WINDOW_OK,
	LP_WINDOW_TOO_SHORT,
	LP_WINDOW_TOO_LONG,
	LP_WINDOW_SUSPENDED,
	LP_WINDOW_CLOCK_JUMP,
};

int64_t lp_guard_next(const struct lp_guard *g, int64_t now_ms);
void lp_guard_record(struct lp_guard *g, int64_t now_ms);

int64_t lp_backoff_ms(int failures);
int64_t lp_window_length_ms(int interval_s);
enum lp_window_status lp_window_check(int interval_s, const struct lp_clock *start, const struct lp_clock *end, int64_t *duration_ms);
const char *lp_window_status_text(enum lp_window_status s);

#endif
