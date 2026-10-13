#ifndef LP_LOG_H
#define LP_LOG_H

enum lp_level {
	LP_ERR = 3,
	LP_WARN = 4,
	LP_INFO = 6,
	LP_DEBUG = 7,
};

void lp_log_init(int verbose);
/* Sends log lines to path, rotated to path.1 at 1 MiB, instead of stderr. */
void lp_log_to_file(const char *path);
void lp_log(enum lp_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
