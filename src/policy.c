#include "policy.h"

int64_t lp_guard_next(const struct lp_guard *g, int64_t now_ms)
{
	int64_t oldest = g->at[0] < g->at[1] ? g->at[0] : g->at[1];

	if (oldest == 0 || now_ms - oldest >= lp_seconds(60))
		return now_ms;
	return oldest + lp_seconds(60);
}

void lp_guard_record(struct lp_guard *g, int64_t now_ms)
{
	if (g->at[0] <= g->at[1])
		g->at[0] = now_ms;
	else
		g->at[1] = now_ms;
}

int64_t lp_backoff_ms(int failures)
{
	int64_t s = 60;

	for (int i = 1; i < failures && s < 300; i++)
		s *= 2;
	if (s > 300)
		s = 300;
	return lp_seconds(s) + lp_jitter_ms(lp_seconds(5));
}

/* The backend caps sample_duration_ms at exactly one hour, so keep a margin at the maximum interval. */
int64_t lp_window_length_ms(int interval_s)
{
	int64_t ms = (int64_t)interval_s * 1000;

	return ms > 3599000 ? 3599000 : ms;
}

enum lp_window_status lp_window_check(int interval_s, const struct lp_clock *start, const struct lp_clock *end, int64_t *duration_ms)
{
	int64_t d = end->mono_ms - start->mono_ms;
	int64_t min = (int64_t)interval_s * 500, max = (int64_t)interval_s * 1500;
	int64_t wall = end->real_ms - start->real_ms;

	if (min < 1000)
		min = 1000;
	if (max > 3600000)
		max = 3600000;
	*duration_ms = d;
	if (start->boot_ms >= 0 && end->boot_ms >= 0 && (end->boot_ms - start->boot_ms) - d > 1000)
		return LP_WINDOW_SUSPENDED;
	if (wall - d > 5000 || d - wall > 5000)
		return LP_WINDOW_CLOCK_JUMP;
	if (d < min)
		return LP_WINDOW_TOO_SHORT;
	if (d > max)
		return LP_WINDOW_TOO_LONG;
	return LP_WINDOW_OK;
}

/*
 * A retry must land before the server would see the next window's report closer than
 * 0.8 x interval, and while observed_at is comfortably inside the 5-minute freshness limit.
 */
bool lp_retry_allowed(int interval_s, int64_t delay_ms, int64_t sample_age_ms)
{
	return delay_ms <= (int64_t)interval_s * 200 - 5000 && sample_age_ms <= 240000;
}

const char *lp_window_status_text(enum lp_window_status s)
{
	switch (s) {
	case LP_WINDOW_OK:
		return "ok";
	case LP_WINDOW_TOO_SHORT:
		return "sample window too short";
	case LP_WINDOW_TOO_LONG:
		return "sample window too long";
	case LP_WINDOW_SUSPENDED:
		return "host was suspended during the window";
	case LP_WINDOW_CLOCK_JUMP:
		return "wall clock changed during the window";
	}
	return "unknown";
}
