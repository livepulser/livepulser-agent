#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"

#define LOG_FILE_LIMIT (1024 * 1024)

static int log_verbose;
static int log_journal;
static char log_path[1100];

void lp_log_init(int verbose)
{
	log_verbose = verbose;
	log_journal = getenv("JOURNAL_STREAM") != NULL;
}

void lp_log_to_file(const char *path)
{
	snprintf(log_path, sizeof log_path, "%s", path);
	log_journal = 0;
}

static FILE *open_log_file(void)
{
	char old[sizeof log_path + 2];
	FILE *f = fopen(log_path, "a");

	if (f && ftell(f) >= LOG_FILE_LIMIT) {
		fclose(f);
		snprintf(old, sizeof old, "%s.1", log_path);
		remove(old);
		rename(log_path, old);
		f = fopen(log_path, "a");
	}
	return f;
}

void lp_log(enum lp_level level, const char *fmt, ...)
{
	static const char *names[] = {[LP_ERR] = "error", [LP_WARN] = "warning", [LP_INFO] = "info", [LP_DEBUG] = "debug"};
	char msg[512], ts[32] = "";
	struct tm tm;
	FILE *out = stderr;
	va_list ap;

	if (level == LP_DEBUG && !log_verbose)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	if (log_journal) {
		fprintf(stderr, "<%d>%s\n", level, msg);
		return;
	}
	if (lp_gmtime((int64_t)time(NULL), &tm))
		strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (log_path[0] && !(out = open_log_file()))
		return;
	fprintf(out, "%s %s: %s\n", ts, names[level], msg);
	if (out != stderr)
		fclose(out);
}
