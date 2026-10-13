#include "state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_FILE "state"
#define STATE_TMP "state.tmp"
#define STATE_CORRUPT "state.corrupt"
#define LOCK_FILE "agent.lock"

static int write_state(struct lp_state *s, int64_t reserved_through, char *err, size_t errlen)
{
	char buf[512];
	int n, fd;
	ssize_t w;

	n = snprintf(buf, sizeof buf, "format_version=1\nmonitor_id=%s\nendpoint=%s\nboot_id=%s\nreserved_through=%lld\n",
		     s->monitor_id, s->endpoint, s->boot_id, (long long)reserved_through);
	if (n < 0 || (size_t)n >= sizeof buf) {
		snprintf(err, errlen, "state record too large");
		return -1;
	}
	fd = openat(s->dir_fd, STATE_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		snprintf(err, errlen, "cannot write state: %s", strerror(errno));
		return -1;
	}
	w = write(fd, buf, (size_t)n);
	if (w != n || fsync(fd) != 0) {
		snprintf(err, errlen, "cannot persist state: %s", w < 0 || w == n ? strerror(errno) : "short write");
		close(fd);
		unlinkat(s->dir_fd, STATE_TMP, 0);
		return -1;
	}
	if (close(fd) != 0 || renameat(s->dir_fd, STATE_TMP, s->dir_fd, STATE_FILE) != 0 || fsync(s->dir_fd) != 0) {
		snprintf(err, errlen, "cannot commit state: %s", strerror(errno));
		return -1;
	}
	s->reserved_through = reserved_through;
	return 0;
}

static int parse_state(struct lp_state *s, char *text, bool *matches)
{
	unsigned seen = 0;
	char monitor[LP_UUID_LEN + 1] = "", endpoint[LP_ENDPOINT_SIZE] = "";
	char *line, *save = NULL;

	for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(line, '='), *end;
		if (!eq)
			return -1;
		*eq++ = '\0';
		if (strcmp(line, "format_version") == 0 && strcmp(eq, "1") == 0) {
			seen |= 1;
		} else if (strcmp(line, "monitor_id") == 0 && strlen(eq) < sizeof monitor) {
			snprintf(monitor, sizeof monitor, "%s", eq);
			seen |= 2;
		} else if (strcmp(line, "endpoint") == 0 && strlen(eq) < sizeof endpoint) {
			snprintf(endpoint, sizeof endpoint, "%s", eq);
			seen |= 4;
		} else if (strcmp(line, "boot_id") == 0 && lp_valid_uuid(eq)) {
			snprintf(s->boot_id, sizeof s->boot_id, "%s", eq);
			seen |= 8;
		} else if (strcmp(line, "reserved_through") == 0) {
			long long v;
			errno = 0;
			v = strtoll(eq, &end, 10);
			if (errno || *end || eq == end || v < 0 || v > LP_MAX_SEQUENCE)
				return -1;
			s->reserved_through = v;
			seen |= 16;
		} else {
			return -1;
		}
	}
	if (seen != 31)
		return -1;
	*matches = strcmp(monitor, s->monitor_id) == 0 && strcmp(endpoint, s->endpoint) == 0;
	return 0;
}

static int load_state(struct lp_state *s)
{
	char text[1025];
	struct stat st;
	ssize_t n;
	bool matches = false;
	int fd = openat(s->dir_fd, STATE_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return errno == ENOENT ? 0 : -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size >= (off_t)sizeof text) {
		close(fd);
		return -1;
	}
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return -1;
	text[n] = '\0';
	if (parse_state(s, text, &matches) != 0)
		return -1;
	return matches ? 1 : 0;
}

int lp_state_open(struct lp_state *s, const char *dir, const char *monitor_id, const char *endpoint, char *err, size_t errlen)
{
	struct stat st;
	int loaded;

	memset(s, 0, sizeof *s);
	s->dir_fd = s->lock_fd = -1;
	snprintf(s->monitor_id, sizeof s->monitor_id, "%s", monitor_id);
	snprintf(s->endpoint, sizeof s->endpoint, "%s", endpoint);

	s->dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (s->dir_fd < 0 || fstat(s->dir_fd, &st) != 0) {
		snprintf(err, errlen, "cannot open state directory %s: %s", dir, strerror(errno));
		goto fail;
	}
	if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
		snprintf(err, errlen, "state directory %s must be owned by the agent user and not group/world writable", dir);
		goto fail;
	}
	s->lock_fd = openat(s->dir_fd, LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (s->lock_fd < 0) {
		snprintf(err, errlen, "cannot open lock file: %s", strerror(errno));
		goto fail;
	}
	if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
		if (errno == EWOULDBLOCK) {
			snprintf(err, errlen, "another agent instance is using %s", dir);
			lp_state_close(s);
			return LP_STATE_LOCKED;
		}
		snprintf(err, errlen, "cannot lock state directory: %s", strerror(errno));
		goto fail;
	}

	loaded = load_state(s);
	if (loaded < 0) {
		s->recovered_corrupt = true;
		if (renameat(s->dir_fd, STATE_FILE, s->dir_fd, STATE_CORRUPT) != 0 && errno != ENOENT) {
			snprintf(err, errlen, "cannot set aside corrupt state: %s", strerror(errno));
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
	if (s->lock_fd >= 0)
		close(s->lock_fd);
	if (s->dir_fd >= 0)
		close(s->dir_fd);
	s->lock_fd = s->dir_fd = -1;
}
