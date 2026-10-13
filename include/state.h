#ifndef LP_STATE_H
#define LP_STATE_H

#include "agent.h"

#define LP_STATE_BLOCK 256

enum {
	LP_STATE_OK = 0,
	LP_STATE_ERROR = -1,
	LP_STATE_LOCKED = -2,
};

struct lp_dir;

struct lp_state {
	struct lp_dir *dir;
	char monitor_id[LP_UUID_LEN + 1];
	char endpoint[LP_ENDPOINT_SIZE];
	char boot_id[LP_UUID_LEN + 1];
	int64_t reserved_through;
	int64_t next_sequence;
	bool new_session;
	bool recovered_corrupt;
};

int lp_state_open(struct lp_state *s, const char *dir, const char *monitor_id, const char *endpoint, char *err, size_t errlen);
int lp_state_take_sequence(struct lp_state *s, int64_t *seq, char *err, size_t errlen);
void lp_state_close(struct lp_state *s);

#endif
