#include "collector.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "platform.h"

#define ROUTE_UNSET (-1)
#define ROUTE_INVALID_NAME (-2)

int lp_collector_init(struct lp_collector *c)
{
	memset(c, 0, sizeof *c);
	c->route_status = ROUTE_UNSET;
	return lp_os_init();
}

void lp_collector_cleanup(void)
{
	lp_os_cleanup();
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
			snprintf(name, sizeof name, "%s", cfg->ifname);
	} else {
		status = lp_os_default_interface(name);
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
		lp_log(LP_WARN, "configured network interface name is not valid on this system; network usage is unavailable");
		break;
	default:
		lp_log(LP_WARN, "no default route found; set network_interface in the dashboard to report network usage");
		break;
	}
	return true;
}

void lp_collect_counters(const struct lp_collector *c, struct lp_counters *out)
{
	memset(out, 0, sizeof *out);
	lp_clock_now(&out->clock);
	if (lp_os_cpu(&out->cpu) != 0)
		out->cpu.valid = false;
	if (c->route_status != LP_ROUTE_FOUND)
		return;
	snprintf(out->ifname, sizeof out->ifname, "%s", c->ifname);
	out->net_valid = lp_os_net(c->ifname, &out->if_id, &out->rx, &out->tx) == 0;
}

void lp_collect_metrics(const struct lp_config *cfg, const struct lp_counters *start, const struct lp_counters *end,
			int64_t duration_ms, struct lp_metrics *m)
{
	memset(m, 0, sizeof *m);
	m->ok[LP_M_CPU] = lp_cpu_percent(&start->cpu, &end->cpu, &m->value[LP_M_CPU]) == 0;
	m->ok[LP_M_MEMORY] = lp_os_memory(&m->value[LP_M_MEMORY]) == 0;
	m->ok[LP_M_SWAP] = lp_os_swap(&m->value[LP_M_SWAP]) == 0;
	m->ok[LP_M_DISK] = cfg->disk_path_valid && lp_os_disk(cfg->disk_path, &m->value[LP_M_DISK]) == 0;
	if (start->net_valid && end->net_valid && start->if_id == end->if_id && strcmp(start->ifname, end->ifname) == 0) {
		m->ok[LP_M_NET_TX] = lp_rate(start->tx, end->tx, duration_ms, &m->value[LP_M_NET_TX]) == 0;
		m->ok[LP_M_NET_RX] = lp_rate(start->rx, end->rx, duration_ms, &m->value[LP_M_NET_RX]) == 0;
	}
}
