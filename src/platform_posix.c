#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

#define TMP_SUFFIX ".tmp"

struct lp_dir {
	int fd;
	int lock_fd;
};

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t reload_flag;
static int wake_pipe[2] = {-1, -1};

const char *lp_os_name(void)
{
#if defined(__APPLE__)
	return "darwin";
#else
	return "linux";
#endif
}

const char *lp_arch(void)
{
	static char arch[33];
	struct utsname u;

	if (arch[0])
		return arch;
	if (uname(&u) != 0)
		snprintf(arch, sizeof arch, "unknown");
	else if (strcmp(u.machine, "x86_64") == 0)
		snprintf(arch, sizeof arch, "amd64");
	else if (strcmp(u.machine, "aarch64") == 0 || strcmp(u.machine, "arm64") == 0)
		snprintf(arch, sizeof arch, "arm64");
	else
		snprintf(arch, sizeof arch, "%.32s", u.machine);
	return arch;
}

const char *lp_default_config_path(void)
{
#if defined(__APPLE__)
	return "/usr/local/etc/livepulser-agent/agent.conf";
#else
	return "/etc/livepulser-agent/agent.conf";
#endif
}

const char *lp_default_state_dir(void)
{
#if defined(__APPLE__)
	return "/usr/local/var/livepulser-agent";
#else
	return "/var/lib/livepulser-agent";
#endif
}

bool lp_path_is_absolute(const char *path)
{
	return path[0] == '/';
}

bool lp_valid_disk_path(const char *path)
{
	return path[0] == '/';
}

bool lp_valid_ifname(const char *name)
{
	size_t n = strlen(name);

	if (n == 0 || n > 15 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		if (c <= ' ' || c >= 0x7f || c == '/' || c == ':')
			return false;
	}
	return true;
}

static int64_t clock_ms(clockid_t id)
{
	struct timespec ts;

	if (clock_gettime(id, &ts) != 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * The measurement clock stops while the host sleeps; the second clock keeps running.
 * A difference between them reveals a suspend inside a sample window.
 */
#if defined(__APPLE__)
#define MEASURE_CLOCK CLOCK_UPTIME_RAW
#define SLEEP_AWARE_CLOCK CLOCK_MONOTONIC
#else
#define MEASURE_CLOCK CLOCK_MONOTONIC
#define SLEEP_AWARE_CLOCK CLOCK_BOOTTIME
#endif

int64_t lp_mono_ms(void)
{
	return clock_ms(MEASURE_CLOCK);
}

void lp_clock_now(struct lp_clock *t)
{
	t->mono_ms = clock_ms(MEASURE_CLOCK);
	t->boot_ms = clock_ms(SLEEP_AWARE_CLOCK);
	t->real_ms = clock_ms(CLOCK_REALTIME);
}

bool lp_gmtime(int64_t unix_s, struct tm *out)
{
	time_t t = (time_t)unix_s;

	return gmtime_r(&t, out) != NULL;
}

int lp_random_bytes(void *buf, size_t n)
{
	unsigned char *p = buf;

	while (n > 0) {
		size_t chunk = n > 256 ? 256 : n;
		if (getentropy(p, chunk) != 0)
			return -1;
		p += chunk;
		n -= chunk;
	}
	return 0;
}

static void wake(void)
{
	if (wake_pipe[1] >= 0 && write(wake_pipe[1], "x", 1) < 0) {
		/* The pipe is non-blocking; a full pipe already guarantees a wakeup. */
	}
}

static void on_signal(int sig)
{
	int saved = errno;

	if (sig == SIGHUP)
		reload_flag = 1;
	else
		stop_flag = 1;
	wake();
	errno = saved;
}

int lp_events_init(void)
{
	struct sigaction sa;

	if (pipe(wake_pipe) != 0)
		return -1;
	for (int i = 0; i < 2; i++) {
		if (fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK) != 0 || fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC) != 0)
			return -1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) || sigaction(SIGHUP, &sa, NULL))
		return -1;
	signal(SIGPIPE, SIG_IGN);
	return 0;
}

void lp_wait_until(int64_t deadline_ms)
{
	struct pollfd pfd = {.fd = wake_pipe[0], .events = POLLIN};
	int timeout = -1;
	char drain[16];

	if (deadline_ms != LP_NEVER) {
		int64_t left = deadline_ms - lp_mono_ms();
		timeout = left <= 0 ? 0 : left > INT_MAX ? INT_MAX : (int)left;
	}
	if (poll(&pfd, 1, timeout) > 0) {
		while (read(wake_pipe[0], drain, sizeof drain) > 0)
			;
	}
}

bool lp_stop_requested(void)
{
	return stop_flag != 0;
}

bool lp_take_reload(void)
{
	if (!reload_flag)
		return false;
	reload_flag = 0;
	return true;
}

void lp_request_stop(void)
{
	stop_flag = 1;
	wake();
}

void lp_request_reload(void)
{
	reload_flag = 1;
	wake();
}

static int read_fd(int fd, const char *path, bool secret, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	struct stat st;
	ssize_t n;

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size >= cap) {
		snprintf(err, errlen, "%s must be a regular file smaller than %zu bytes", path, cap);
		return -1;
	}
	if (secret && (st.st_mode & (S_IRWXO | S_IWGRP))) {
		snprintf(err, errlen, "%s must not be accessible by other users (chmod 0600 or 0640)", path);
		return -1;
	}
	n = read(fd, buf, cap - 1);
	if (n < 0) {
		snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	buf[n] = '\0';
	*len = (size_t)n;
	return 0;
}

static int read_path(const char *path, bool secret, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY), rc;

	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	rc = read_fd(fd, path, secret, buf, cap, len, err, errlen);
	close(fd);
	return rc;
}

int lp_read_config_file(const char *path, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	return read_path(path, false, buf, cap, len, err, errlen);
}

int lp_read_secret_file(const char *path, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	return read_path(path, true, buf, cap, len, err, errlen);
}

int lp_dir_open_locked(const char *path, struct lp_dir **out, char *err, size_t errlen)
{
	struct lp_dir *d = calloc(1, sizeof *d);
	struct stat st;

	if (!d) {
		snprintf(err, errlen, "out of memory");
		return LP_DIR_ERROR;
	}
	d->lock_fd = -1;
	d->fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (d->fd < 0 || fstat(d->fd, &st) != 0) {
		snprintf(err, errlen, "cannot open state directory %s: %s", path, strerror(errno));
		goto fail;
	}
	if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
		snprintf(err, errlen, "state directory %s must be owned by the agent user and not group/world writable", path);
		goto fail;
	}
	d->lock_fd = openat(d->fd, "agent.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (d->lock_fd < 0) {
		snprintf(err, errlen, "cannot open lock file: %s", strerror(errno));
		goto fail;
	}
	if (flock(d->lock_fd, LOCK_EX | LOCK_NB) != 0) {
		bool busy = errno == EWOULDBLOCK;
		snprintf(err, errlen, busy ? "another agent instance is using %s" : "cannot lock %s", path);
		lp_dir_close(d);
		return busy ? LP_DIR_LOCKED : LP_DIR_ERROR;
	}
	*out = d;
	return LP_DIR_OK;
fail:
	lp_dir_close(d);
	return LP_DIR_ERROR;
}

int lp_dir_read(struct lp_dir *d, const char *name, char *buf, size_t cap, size_t *len)
{
	char err[64];
	int fd = openat(d->fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), rc;

	if (fd < 0)
		return errno == ENOENT ? LP_DIR_MISSING : LP_DIR_ERROR;
	rc = read_fd(fd, name, false, buf, cap, len, err, sizeof err);
	close(fd);
	return rc == 0 ? LP_DIR_OK : LP_DIR_ERROR;
}

int lp_dir_write_atomic(struct lp_dir *d, const char *name, const char *data, size_t len, char *err, size_t errlen)
{
	char tmp[64];
	ssize_t w;
	int fd;

	snprintf(tmp, sizeof tmp, "%s" TMP_SUFFIX, name);
	fd = openat(d->fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		snprintf(err, errlen, "cannot write state: %s", strerror(errno));
		return -1;
	}
	w = write(fd, data, len);
	if (w < 0 || (size_t)w != len || fsync(fd) != 0) {
		snprintf(err, errlen, "cannot persist state: %s", w >= 0 && (size_t)w != len ? "short write" : strerror(errno));
		close(fd);
		unlinkat(d->fd, tmp, 0);
		return -1;
	}
	if (close(fd) != 0 || renameat(d->fd, tmp, d->fd, name) != 0 || fsync(d->fd) != 0) {
		snprintf(err, errlen, "cannot commit state: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int lp_dir_rename(struct lp_dir *d, const char *from, const char *to)
{
	if (renameat(d->fd, from, d->fd, to) == 0)
		return LP_DIR_OK;
	return errno == ENOENT ? LP_DIR_MISSING : LP_DIR_ERROR;
}

void lp_dir_close(struct lp_dir *d)
{
	if (!d)
		return;
	if (d->lock_fd >= 0)
		close(d->lock_fd);
	if (d->fd >= 0)
		close(d->fd);
	free(d);
}
