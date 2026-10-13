#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int log_verbose;
static int log_journal;

void lp_log_init(int verbose)
{
	log_verbose = verbose;
	log_journal = getenv("JOURNAL_STREAM") != NULL;
}

void lp_log(enum lp_level level, const char *fmt, ...)
{
	static const char *names[] = {[LP_ERR] = "error", [LP_WARN] = "warning", [LP_INFO] = "info", [LP_DEBUG] = "debug"};
	char msg[512];
	va_list ap;

	if (level == LP_DEBUG && !log_verbose)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	if (log_journal) {
		fprintf(stderr, "<%d>%s\n", level, msg);
	} else {
		char ts[32] = "";
		time_t now = time(NULL);
		struct tm tm;
		if (gmtime_r(&now, &tm))
			strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
		fprintf(stderr, "%s %s: %s\n", ts, names[level], msg);
	}
}
