#include "options.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_OPTIONS_BYTES 16384

static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static bool loopback_host(const char *host, size_t len)
{
	static const char *const names[] = {"127.0.0.1", "localhost", "[::1]"};

	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		size_t n = strlen(names[i]);
		if (len >= n && strncmp(host, names[i], n) == 0 && (host[n] == '\0' || host[n] == ':'))
			return true;
	}
	return false;
}

static int set_endpoint(struct lp_options *o, char *value, char *err, size_t errlen)
{
	size_t n = strlen(value);
	const char *host;
	bool https = strncmp(value, "https://", 8) == 0;

	while (n > 0 && value[n - 1] == '/')
		value[--n] = '\0';
	host = value + (https ? 8 : 7);
	if (!https && strncmp(value, "http://", 7) != 0) {
		snprintf(err, errlen, "endpoint must start with https://");
		return -1;
	}
	if (n >= sizeof o->endpoint || *host == '\0' || strpbrk(host, "/?#@ \t\\")) {
		snprintf(err, errlen, "endpoint must be an origin such as https://rs.livepulser.com");
		return -1;
	}
	if (!https && !loopback_host(host, strlen(host))) {
		snprintf(err, errlen, "plain http endpoints are only allowed for loopback testing");
		return -1;
	}
	memcpy(o->endpoint, value, n + 1);
	return 0;
}

static int set_path(char *dst, size_t cap, const char *key, const char *value, char *err, size_t errlen)
{
	if (value[0] != '/' || strlen(value) >= cap) {
		snprintf(err, errlen, "%s must be an absolute path", key);
		return -1;
	}
	snprintf(dst, cap, "%s", value);
	return 0;
}

static int set_option(struct lp_options *o, const char *key, char *value, char *err, size_t errlen)
{
	if (strcmp(key, "endpoint") == 0)
		return set_endpoint(o, value, err, errlen);
	if (strcmp(key, "monitor_id") == 0) {
		if (!lp_valid_uuid(value)) {
			snprintf(err, errlen, "monitor_id must be a UUID");
			return -1;
		}
		for (int i = 0; value[i]; i++)
			o->monitor_id[i] = (char)tolower((unsigned char)value[i]);
		o->monitor_id[LP_UUID_LEN] = '\0';
		return 0;
	}
	if (strcmp(key, "api_key_file") == 0)
		return set_path(o->api_key_file, sizeof o->api_key_file, key, value, err, errlen);
	if (strcmp(key, "state_dir") == 0)
		return set_path(o->state_dir, sizeof o->state_dir, key, value, err, errlen);
	if (strcmp(key, "ca_file") == 0)
		return set_path(o->ca_file, sizeof o->ca_file, key, value, err, errlen);
	if (strcmp(key, "proxy") == 0) {
		if (strlen(value) >= sizeof o->proxy || strpbrk(value, " \t")) {
			snprintf(err, errlen, "invalid proxy");
			return -1;
		}
		snprintf(o->proxy, sizeof o->proxy, "%s", value);
		return 0;
	}
	snprintf(err, errlen, "unknown option '%.64s'", key);
	return -1;
}

int lp_options_parse(const char *text, size_t len, struct lp_options *o, char *err, size_t errlen)
{
	static const char *const keys[] = {"endpoint", "monitor_id", "api_key_file", "state_dir", "ca_file", "proxy"};
	unsigned seen = 0;
	char line[LP_FILE_PATH_SIZE + 64];
	size_t pos = 0;
	int lineno = 0;

	memset(o, 0, sizeof *o);
	if (memchr(text, '\0', len)) {
		snprintf(err, errlen, "configuration contains a NUL byte");
		return -1;
	}
	while (pos < len) {
		size_t end = pos;
		char *eq, *key, *value;
		unsigned bit = 0;

		while (end < len && text[end] != '\n')
			end++;
		lineno++;
		if (end - pos >= sizeof line) {
			snprintf(err, errlen, "line %d is too long", lineno);
			return -1;
		}
		memcpy(line, text + pos, end - pos);
		line[end - pos] = '\0';
		pos = end + 1;

		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;
		if (!(eq = strchr(key, '='))) {
			snprintf(err, errlen, "line %d: expected key=value", lineno);
			return -1;
		}
		*eq = '\0';
		key = trim(key);
		value = trim(eq + 1);
		for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
			if (strcmp(key, keys[i]) == 0)
				bit = 1u << i;
		}
		if (bit & seen) {
			snprintf(err, errlen, "line %d: duplicate option '%s'", lineno, key);
			return -1;
		}
		seen |= bit;
		if (set_option(o, key, value, err, errlen) != 0)
			return -1;
	}
	return 0;
}

int lp_options_load(const char *path, struct lp_options *o, char *err, size_t errlen)
{
	char text[MAX_OPTIONS_BYTES + 1];
	const char *env;
	ssize_t n;
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);

	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_OPTIONS_BYTES) {
		snprintf(err, errlen, "%s is not a regular file under %d bytes", path, MAX_OPTIONS_BYTES);
		close(fd);
		return -1;
	}
	n = read(fd, text, MAX_OPTIONS_BYTES);
	close(fd);
	if (n < 0) {
		snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	if (lp_options_parse(text, (size_t)n, o, err, errlen) != 0)
		return -1;

	if (!o->api_key_file[0] && (env = getenv("CREDENTIALS_DIRECTORY")) && *env == '/')
		snprintf(o->api_key_file, sizeof o->api_key_file, "%s/api-key", env);
	if (!o->state_dir[0] && (env = getenv("STATE_DIRECTORY")) && *env == '/')
		snprintf(o->state_dir, sizeof o->state_dir, "%.*s", (int)strcspn(env, ":"), env);
	if (!o->state_dir[0])
		snprintf(o->state_dir, sizeof o->state_dir, "%s", LP_DEFAULT_STATE_DIR);

	if (!o->endpoint[0] || !o->monitor_id[0] || !o->api_key_file[0]) {
		snprintf(err, errlen, "%s must set endpoint, monitor_id and api_key_file", path);
		return -1;
	}
	return 0;
}

int lp_api_key_load(const char *path, char key[LP_API_KEY_LEN + 1], char *err, size_t errlen)
{
	char buf[257];
	char *start, *end;
	struct stat st;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY), rc = -1;

	if (fd < 0) {
		snprintf(err, errlen, "cannot open API key file %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 256) {
		snprintf(err, errlen, "API key file %s must be a small regular file", path);
		goto out;
	}
	if (st.st_mode & (S_IRWXO | S_IWGRP)) {
		snprintf(err, errlen, "API key file %s must not be accessible by other users (chmod 0600 or 0640)", path);
		goto out;
	}
	n = read(fd, buf, sizeof buf - 1);
	if (n < 0) {
		snprintf(err, errlen, "cannot read API key file %s: %s", path, strerror(errno));
		goto out;
	}
	buf[n] = '\0';
	start = trim(buf);
	end = start + strlen(start);
	if (end - start != LP_API_KEY_LEN || strspn(start, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != LP_API_KEY_LEN) {
		snprintf(err, errlen, "API key file %s does not contain a valid resource API key", path);
		goto out;
	}
	memcpy(key, start, LP_API_KEY_LEN);
	key[LP_API_KEY_LEN] = '\0';
	rc = 0;
out:
	lp_wipe(buf, sizeof buf);
	close(fd);
	return rc;
}
