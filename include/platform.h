#ifndef LP_PLATFORM_H
#define LP_PLATFORM_H

#include <time.h>

#include "agent.h"

#define LP_NEVER INT64_MAX

enum {
	LP_DIR_OK = 0,
	LP_DIR_ERROR = -1,
	LP_DIR_LOCKED = -2,
	LP_DIR_MISSING = 1,
};

struct lp_dir;

const char *lp_os_name(void);
const char *lp_arch(void);
const char *lp_default_config_path(void);
const char *lp_default_state_dir(void);
bool lp_path_is_absolute(const char *path);
bool lp_valid_ifname(const char *name);
bool lp_valid_disk_path(const char *path);

void lp_clock_now(struct lp_clock *t);
int64_t lp_mono_ms(void);
bool lp_gmtime(int64_t unix_s, struct tm *out);

int lp_events_init(void);
void lp_wait_until(int64_t deadline_ms);
bool lp_stop_requested(void);
bool lp_take_reload(void);
void lp_request_stop(void);
void lp_request_reload(void);

int lp_read_config_file(const char *path, char *buf, size_t cap, size_t *len, char *err, size_t errlen);
/* Like lp_read_config_file, but refuses files that other users can read. */
int lp_read_secret_file(const char *path, char *buf, size_t cap, size_t *len, char *err, size_t errlen);

int lp_dir_open_locked(const char *path, struct lp_dir **out, char *err, size_t errlen);
int lp_dir_read(struct lp_dir *d, const char *name, char *buf, size_t cap, size_t *len);
int lp_dir_write_atomic(struct lp_dir *d, const char *name, const char *data, size_t len, char *err, size_t errlen);
int lp_dir_rename(struct lp_dir *d, const char *from, const char *to);
void lp_dir_close(struct lp_dir *d);

#endif
