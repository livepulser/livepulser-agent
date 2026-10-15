#ifndef LP_AGENT_H
#define LP_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LP_AGENT_NAME "livepulser-agent"
#ifndef LP_VERSION
#define LP_VERSION "0.2.1"
#endif

enum {
	LP_EXIT_OK = 0,
	LP_EXIT_RUNTIME = 1,
	LP_EXIT_AUTH = 77,
	LP_EXIT_CONFIG = 78,
};

#define LP_MAX_SEQUENCE 9007199254740991LL
#define LP_MAX_REPORT_BYTES 8192
#define LP_MAX_CONFIG_BYTES 16384
#define LP_MAX_SAMPLES 60
#define LP_API_KEY_LEN 43
#define LP_UUID_LEN 36
#define LP_IFNAME_SIZE 65
#define LP_ENDPOINT_SIZE 257
#define LP_DISK_PATH_SIZE 257
#define LP_FILE_PATH_SIZE 1024

#ifdef LP_TESTING
extern int64_t lp_time_divisor;
#define LP_MIN_INTERVAL_S 1
#else
#define LP_MIN_INTERVAL_S 30
#endif
#define LP_MAX_INTERVAL_S 3600

/* Protocol timing constants; test builds compress them so suites finish quickly. */
static inline int64_t lp_seconds(int64_t s)
{
#ifdef LP_TESTING
	int64_t ms = s * 1000 / lp_time_divisor;
	return ms > 0 ? ms : 1;
#else
	return s * 1000;
#endif
}

struct lp_clock {
	int64_t mono_ms;
	int64_t boot_ms;
	int64_t real_ms;
};

void lp_wipe(void *p, size_t n);
int lp_random_bytes(void *buf, size_t n);
int64_t lp_jitter_ms(int64_t max_ms);
int lp_uuid4(char out[LP_UUID_LEN + 1]);
bool lp_valid_uuid(const char *s);
int64_t lp_parse_http_date(const char *s);
int64_t lp_parse_retry_after(const char *value, int64_t now_unix);

#endif
