#include "collector.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "log.h"

#define ROUTE_UNSET (-1)
#define ROUTE_INVALID_NAME (-2)

static FILE *open_proc(const struct lp_collector *c, const char *name)
{
	char path[320];

	snprintf(path, sizeof path, "%s/%s", c->proc_root, name);
	return fopen(path, "re");
}

static int64_t clock_ms(clockid_t id)
{
	struct timespec ts;

	if (clock_gettime(id, &ts) != 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t lp_mono_ms(void)
{
	return clock_ms(CLOCK_MONOTONIC);
}

void lp_clock_now(struct lp_clock *t)
{
	t->mono_ms = clock_ms(CLOCK_MONOTONIC);
#ifdef CLOCK_BOOTTIME
	t->boot_ms = clock_ms(CLOCK_BOOTTIME);
#else
	t->boot_ms = -1;
#endif
	t->real_ms = clock_ms(CLOCK_REALTIME);
}

void lp_collector_init(struct lp_collector *c)
{
	memset(c, 0, sizeof *c);
	snprintf(c->proc_root, sizeof c->proc_root, "/proc");
#ifdef LP_TESTING
	const char *root = getenv("LP_TEST_PROC_ROOT");
	if (root && *root)
		snprintf(c->proc_root, sizeof c->proc_root, "%s", root);
#endif
	c->route_status = ROUTE_UNSET;
}

static int resolve_default(const struct lp_collector *c, char name[LP_IFNAME_SIZE])
{
	enum lp_route_result r = LP_ROUTE_NONE;
	FILE *f;

	if ((f = open_proc(c, "net/route"))) {
		r = lp_route_v4(f, name);
		fclose(f);
	}
	if (r == LP_ROUTE_NONE && (f = open_proc(c, "net/ipv6_route"))) {
		r = lp_route_v6(f, name);
		fclose(f);
	}
	return r;
}

bool lp_collector_select(struct lp_collector *c, const struct lp_config *cfg, int64_t now_ms, bool force)
{
	char name[LP_IFNAME_SIZE] = "";
	int status;

	if (!force && c->route_status != ROUTE_UNSET && now_ms - c->route_checked_ms < lp_seconds(300))
		return false;
	c->route_checked_ms = now_ms;
	if (cfg->interface_explicit) {
		status = cfg->interface_valid ? LP_ROUTE_FOUND : ROUTE_INVALID_NAME;
		if (cfg->interface_valid)
			snprintf(name, sizeof name, "%s", cfg->interface);
	} else {
		status = resolve_default(c, name);
	}
	if (status == c->route_status && strcmp(name, c->ifname) == 0)
		return false;

	c->route_status = status;
	snprintf(c->ifname, sizeof c->ifname, "%s", name);
	switch (status) {
	case LP_ROUTE_FOUND:
		lp_log(LP_INFO, "measuring network interface %s%s", name, cfg->interface_explicit ? "" : " (default route)");
		break;
	case LP_ROUTE_AMBIGUOUS:
		lp_log(LP_WARN, "several default routes have equal priority; set network_interface in the dashboard to report network usage");
		break;
	case ROUTE_INVALID_NAME:
		lp_log(LP_WARN, "configured network interface name is not valid on Linux; network usage is unavailable");
		break;
	default:
		lp_log(LP_WARN, "no default route found; set network_interface in the dashboard to report network usage");
		break;
	}
	return true;
}

void lp_collect_counters(const struct lp_collector *c, struct lp_counters *out)
{
	FILE *f;

	memset(out, 0, sizeof *out);
	lp_clock_now(&out->clock);
	if ((f = open_proc(c, "stat"))) {
		if (lp_parse_stat(f, &out->cpu) != 0)
			out->cpu.valid = false;
		fclose(f);
	}
	if (c->route_status != LP_ROUTE_FOUND)
		return;
	snprintf(out->ifname, sizeof out->ifname, "%s", c->ifname);
	out->ifindex = if_nametoindex(c->ifname);
	if (out->ifindex != 0 && (f = open_proc(c, "net/dev"))) {
		out->net_valid = lp_parse_netdev(f, c->ifname, &out->rx, &out->tx) == 0;
		fclose(f);
	}
}

void lp_collect_metrics(const struct lp_collector *c, const struct lp_config *cfg, const struct lp_counters *start,
			const struct lp_counters *end, int64_t duration_ms, struct lp_metrics *m)
{
	struct lp_meminfo mem;
	struct statvfs vfs;
	FILE *f;

	memset(m, 0, sizeof *m);
	m->ok[LP_M_CPU] = lp_cpu_percent(&start->cpu, &end->cpu, &m->value[LP_M_CPU]) == 0;

	if ((f = open_proc(c, "meminfo"))) {
		if (lp_parse_meminfo(f, &mem) == 0) {
			m->ok[LP_M_MEMORY] = lp_memory_percent(&mem, &m->value[LP_M_MEMORY]) == 0;
			m->ok[LP_M_SWAP] = lp_swap_percent(&mem, &m->value[LP_M_SWAP]) == 0;
		}
		fclose(f);
	}

	if (cfg->disk_path_valid && statvfs(cfg->disk_path, &vfs) == 0)
		m->ok[LP_M_DISK] = lp_disk_percent((uint64_t)vfs.f_blocks, (uint64_t)vfs.f_bfree, &m->value[LP_M_DISK]) == 0;

	if (start->net_valid && end->net_valid && start->ifindex == end->ifindex && strcmp(start->ifname, end->ifname) == 0) {
		m->ok[LP_M_NET_TX] = lp_rate(start->tx, end->tx, duration_ms, &m->value[LP_M_NET_TX]) == 0;
		m->ok[LP_M_NET_RX] = lp_rate(start->rx, end->rx, duration_ms, &m->value[LP_M_NET_RX]) == 0;
	}
}
