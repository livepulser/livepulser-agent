#include "state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define STATE_FILE "state"
#define STATE_CORRUPT "state.corrupt"

static int write_state(struct lp_state *s, int64_t reserved_through, char *err, size_t errlen)
{
	char buf[512];
	int n = snprintf(buf, sizeof buf, "format_version=1\nmonitor_id=%s\nendpoint=%s\nboot_id=%s\nreserved_through=%lld\n",
			 s->monitor_id, s->endpoint, s->boot_id, (long long)reserved_through);

	if (n < 0 || (size_t)n >= sizeof buf) {
		snprintf(err, errlen, "state record too large");
		return -1;
	}
	if (lp_dir_write_atomic(s->dir, STATE_FILE, buf, (size_t)n, err, errlen) != 0)
		return -1;
	s->reserved_through = reserved_through;
	return 0;
}

static int parse_line(struct lp_state *s, char *line, unsigned *seen, char *monitor, char *endpoint)
{
	char *eq = strchr(line, '='), *end;
	long long v;

	if (!eq)
		return -1;
	*eq++ = '\0';
	if (strcmp(line, "format_version") == 0 && strcmp(eq, "1") == 0) {
		*seen |= 1;
	} else if (strcmp(line, "monitor_id") == 0 && strlen(eq) <= LP_UUID_LEN) {
		strcpy(monitor, eq);
		*seen |= 2;
	} else if (strcmp(line, "endpoint") == 0 && strlen(eq) < LP_ENDPOINT_SIZE) {
		strcpy(endpoint, eq);
		*seen |= 4;
	} else if (strcmp(line, "boot_id") == 0 && lp_valid_uuid(eq)) {
		strcpy(s->boot_id, eq);
		*seen |= 8;
	} else if (strcmp(line, "reserved_through") == 0) {
		errno = 0;
		v = strtoll(eq, &end, 10);
		if (errno || *end || eq == end || v < 0 || v > LP_MAX_SEQUENCE)
			return -1;
		s->reserved_through = v;
		*seen |= 16;
	} else {
		return -1;
	}
	return 0;
}

/* 1: valid state for this monitor, 0: absent or for another monitor, -1: corrupt. */
static int load_state(struct lp_state *s)
{
	char text[1025], monitor[LP_UUID_LEN + 1] = "", endpoint[LP_ENDPOINT_SIZE] = "";
	unsigned seen = 0;
	size_t len;
	char *line;
	int rc = lp_dir_read(s->dir, STATE_FILE, text, sizeof text, &len);

	if (rc == LP_DIR_MISSING)
		return 0;
	if (rc != LP_DIR_OK || len == 0)
		return -1;
	for (line = text; *line;) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		if (*line && parse_line(s, line, &seen, monitor, endpoint) != 0)
			return -1;
		if (!nl)
			break;
		line = nl + 1;
	}
	if (seen != 31)
		return -1;
	return strcmp(monitor, s->monitor_id) == 0 && strcmp(endpoint, s->endpoint) == 0;
}

int lp_state_open(struct lp_state *s, const char *dir, const char *monitor_id, const char *endpoint, char *err, size_t errlen)
{
	int loaded, rc;

	memset(s, 0, sizeof *s);
	snprintf(s->monitor_id, sizeof s->monitor_id, "%s", monitor_id);
	snprintf(s->endpoint, sizeof s->endpoint, "%s", endpoint);

	rc = lp_dir_open_locked(dir, &s->dir, err, errlen);
	if (rc != LP_DIR_OK)
		return rc == LP_DIR_LOCKED ? LP_STATE_LOCKED : LP_STATE_ERROR;

	loaded = load_state(s);
	if (loaded < 0) {
		s->recovered_corrupt = true;
		if (lp_dir_rename(s->dir, STATE_FILE, STATE_CORRUPT) == LP_DIR_ERROR) {
			snprintf(err, errlen, "cannot set aside corrupt state file");
			goto fail;
		}
	}
	if (loaded != 1) {
		s->new_session = true;
		s->reserved_through = 0;
		if (lp_uuid4(s->boot_id) != 0) {
			snprintf(err, errlen, "cannot read system randomness");
			goto fail;
		}
		if (write_state(s, 0, err, errlen) != 0)
			goto fail;
	}
	/* Numbers reserved by a previous run may have been sent; never reuse them. */
	s->next_sequence = s->reserved_through + 1;
	return LP_STATE_OK;
fail:
	lp_state_close(s);
	return LP_STATE_ERROR;
}

int lp_state_take_sequence(struct lp_state *s, int64_t *seq, char *err, size_t errlen)
{
	if (s->next_sequence > LP_MAX_SEQUENCE) {
		snprintf(err, errlen, "sequence space exhausted");
		return -1;
	}
	if (s->next_sequence > s->reserved_through) {
		int64_t through = s->next_sequence + LP_STATE_BLOCK - 1;
		if (through > LP_MAX_SEQUENCE)
			through = LP_MAX_SEQUENCE;
		if (write_state(s, through, err, errlen) != 0)
			return -1;
	}
	*seq = s->next_sequence++;
	return 0;
}

void lp_state_close(struct lp_state *s)
{
	lp_dir_close(s->dir);
	s->dir = NULL;
}
