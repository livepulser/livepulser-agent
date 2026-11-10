#include "procfs.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define RTF_UP_FLAG 0x0001
#define RTF_REJECT_FLAG 0x0200

enum {
	HAVE_MEM_TOTAL = 1,
	HAVE_MEM_AVAILABLE = 2,
	HAVE_SWAP_TOTAL = 4,
	HAVE_SWAP_FREE = 8,
	HAVE_ALL = 15,
};

/* 1: complete line, 0: EOF, -1: oversized line (consumed and skipped). */
static int read_line(FILE *f, char *buf, size_t cap)
{
	size_t n;
	int c;

	if (!fgets(buf, (int)cap, f))
		return 0;
	n = strlen(buf);
	if (n && buf[n - 1] == '\n') {
		buf[n - 1] = '\0';
		return 1;
	}
	if (feof(f))
		return 1;
	while ((c = fgetc(f)) != EOF && c != '\n')
		;
	return -1;
}

static char *next_token(char **cursor)
{
	char *p = *cursor, *start;

	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return NULL;
	start = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;
	if (*p)
		*p++ = '\0';
	*cursor = p;
	return start;
}

static int parse_u64(const char *s, int base, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (!s || !isxdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoull(s, &end, base);
	if (errno || *end)
		return -1;
	*out = v;
	return 0;
}

static int add_u64(uint64_t *acc, uint64_t v)
{
	if (*acc > UINT64_MAX - v)
		return -1;
	*acc += v;
	return 0;
}

int lp_parse_stat(FILE *f, struct lp_cpu *out)
{
	char line[512];
	int r, lines = 0;

	memset(out, 0, sizeof *out);
	while (lines++ < 8192 && (r = read_line(f, line, sizeof line)) != 0) {
		/* CPU lines come first; stop before the potentially huge intr line. */
		if (r < 0 || strncmp(line, "cpu", 3) != 0) {
			if (out->valid)
				break;
			continue;
		}
		if (isdigit((unsigned char)line[3])) {
			out->ncpu++;
		} else if (line[3] == ' ' && !out->valid) {
			char *cursor = line + 3;
			for (int i = 0; i < 8; i++) {
				if (parse_u64(next_token(&cursor), 10, &out->field[i]) || add_u64(&out->total, out->field[i]))
					return -1;
			}
			out->idle = out->field[3];
			if (add_u64(&out->idle, out->field[4]))
				return -1;
			/* The kernel documents that iowait can go backwards; aggregate checks still apply. */
			out->may_decrease = 1u << 4;
			out->valid = true;
		}
	}
	return out->valid && out->ncpu > 0 ? 0 : -1;
}

int lp_parse_meminfo(FILE *f, struct lp_meminfo *out)
{
	static const struct {
		const char *name;
		unsigned flag;
		size_t offset;
	} keys[] = {
		{"MemTotal", HAVE_MEM_TOTAL, offsetof(struct lp_meminfo, mem_total)},
		{"MemAvailable", HAVE_MEM_AVAILABLE, offsetof(struct lp_meminfo, mem_available)},
		{"SwapTotal", HAVE_SWAP_TOTAL, offsetof(struct lp_meminfo, swap_total)},
		{"SwapFree", HAVE_SWAP_FREE, offsetof(struct lp_meminfo, swap_free)},
	};
	char line[256];
	int r, lines = 0;

	memset(out, 0, sizeof *out);
	while (out->have != HAVE_ALL && lines++ < 256 && (r = read_line(f, line, sizeof line)) != 0) {
		char *colon, *cursor;
		if (r < 0 || !(colon = strchr(line, ':')))
			continue;
		*colon = '\0';
		for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
			uint64_t v;
			char *unit;
			if (strcmp(line, keys[i].name) != 0 || (out->have & keys[i].flag))
				continue;
			cursor = colon + 1;
			if (parse_u64(next_token(&cursor), 10, &v))
				break;
			unit = next_token(&cursor);
			if (unit && strcmp(unit, "kB") != 0)
				break;
			*(uint64_t *)((char *)out + keys[i].offset) = v;
			out->have |= keys[i].flag;
		}
	}
	return out->have ? 0 : -1;
}

int lp_memory_percent(const struct lp_meminfo *m, double *out)
{
	if ((m->have & (HAVE_MEM_TOTAL | HAVE_MEM_AVAILABLE)) != (HAVE_MEM_TOTAL | HAVE_MEM_AVAILABLE))
		return -1;
	return lp_used_percent(m->mem_total, m->mem_available, out);
}

int lp_swap_percent(const struct lp_meminfo *m, double *out)
{
	if ((m->have & (HAVE_SWAP_TOTAL | HAVE_SWAP_FREE)) != (HAVE_SWAP_TOTAL | HAVE_SWAP_FREE))
		return -1;
	return lp_used_percent(m->swap_total, m->swap_free, out);
}

int lp_parse_netdev(FILE *f, const char *ifname, uint64_t *rx, uint64_t *tx)
{
	char line[512];
	int r, lines = 0;

	while (lines++ < 4096 && (r = read_line(f, line, sizeof line)) != 0) {
		char *colon, *name, *cursor;
		uint64_t v[9];
		if (r < 0 || !(colon = strchr(line, ':')))
			continue;
		*colon = '\0';
		cursor = line;
		name = next_token(&cursor);
		if (!name || strcmp(name, ifname) != 0 || next_token(&cursor))
			continue;
		cursor = colon + 1;
		for (int i = 0; i < 9; i++) {
			if (parse_u64(next_token(&cursor), 10, &v[i]))
				return -1;
		}
		*rx = v[0];
		*tx = v[8];
		return 0;
	}
	return -1;
}

struct route_pick {
	bool found;
	bool ambiguous;
	uint64_t metric;
	char name[LP_IFNAME_SIZE];
};

static void consider(struct route_pick *p, const char *name, uint64_t metric)
{
	if (!lp_valid_ifname(name) || strcmp(name, "lo") == 0)
		return;
	if (!p->found || metric < p->metric) {
		p->found = true;
		p->ambiguous = false;
		p->metric = metric;
		snprintf(p->name, sizeof p->name, "%s", name);
	} else if (metric == p->metric && strcmp(name, p->name) != 0) {
		p->ambiguous = true;
	}
}

static enum lp_route_result finish(const struct route_pick *p, char out[LP_IFNAME_SIZE])
{
	if (!p->found)
		return LP_ROUTE_NONE;
	if (p->ambiguous)
		return LP_ROUTE_AMBIGUOUS;
	snprintf(out, LP_IFNAME_SIZE, "%s", p->name);
	return LP_ROUTE_FOUND;
}

enum lp_route_result lp_route_v4(FILE *f, char out[LP_IFNAME_SIZE])
{
	struct route_pick pick = {0};
	char line[256];
	int r, lines = 0;

	while (lines++ < 4096 && (r = read_line(f, line, sizeof line)) != 0) {
		char *cursor = line, *tok[8];
		uint64_t dest, flags, metric, mask;
		int n = 0;
		if (r < 0)
			continue;
		while (n < 8 && (tok[n] = next_token(&cursor)))
			n++;
		if (n < 8 || parse_u64(tok[1], 16, &dest) || parse_u64(tok[3], 16, &flags) ||
		    parse_u64(tok[6], 10, &metric) || parse_u64(tok[7], 16, &mask))
			continue;
		if (dest == 0 && mask == 0 && (flags & RTF_UP_FLAG) && !(flags & RTF_REJECT_FLAG))
			consider(&pick, tok[0], metric);
	}
	return finish(&pick, out);
}

static bool all_zero(const char *s, size_t len)
{
	if (strlen(s) != len)
		return false;
	for (size_t i = 0; i < len; i++) {
		if (s[i] != '0')
			return false;
	}
	return true;
}

enum lp_route_result lp_route_v6(FILE *f, char out[LP_IFNAME_SIZE])
{
	struct route_pick pick = {0};
	char line[256];
	int r, lines = 0;

	while (lines++ < 8192 && (r = read_line(f, line, sizeof line)) != 0) {
		char *cursor = line, *tok[10];
		uint64_t metric, flags;
		int n = 0;
		if (r < 0)
			continue;
		while (n < 10 && (tok[n] = next_token(&cursor)))
			n++;
		if (n < 10 || parse_u64(tok[5], 16, &metric) || parse_u64(tok[8], 16, &flags))
			continue;
		if (all_zero(tok[0], 32) && all_zero(tok[1], 2) && all_zero(tok[3], 2) &&
		    (flags & RTF_UP_FLAG) && !(flags & RTF_REJECT_FLAG))
			consider(&pick, tok[9], metric);
	}
	return finish(&pick, out);
}
