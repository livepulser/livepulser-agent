#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "collector.h"
#include "procfs.h"

static char proc_root[256] = "/proc";

static FILE *open_proc(const char *name)
{
	char path[320];

	snprintf(path, sizeof path, "%s/%s", proc_root, name);
	return fopen(path, "re");
}

int lp_os_init(void)
{
#ifdef LP_TESTING
	const char *root = getenv("LP_TEST_PROC_ROOT");
	if (root && *root)
		snprintf(proc_root, sizeof proc_root, "%s", root);
#endif
	return 0;
}

void lp_os_cleanup(void)
{
}

int lp_os_cpu(struct lp_cpu *out)
{
	FILE *f = open_proc("stat");
	int rc;

	if (!f)
		return -1;
	rc = lp_parse_stat(f, out);
	fclose(f);
	return rc;
}

static int read_meminfo(struct lp_meminfo *m)
{
	FILE *f = open_proc("meminfo");
	int rc;

	if (!f)
		return -1;
	rc = lp_parse_meminfo(f, m);
	fclose(f);
	return rc;
}

int lp_os_memory(double *out)
{
	struct lp_meminfo m;

	return read_meminfo(&m) == 0 ? lp_memory_percent(&m, out) : -1;
}

int lp_os_swap(double *out)
{
	struct lp_meminfo m;

	return read_meminfo(&m) == 0 ? lp_swap_percent(&m, out) : -1;
}

int lp_os_disk(const char *path, double *out)
{
	struct statvfs vfs;

	if (statvfs(path, &vfs) != 0)
		return -1;
	return lp_used_percent((uint64_t)vfs.f_blocks, (uint64_t)vfs.f_bfree, out);
}

int lp_os_net(const char *ifname, uint64_t *id, uint64_t *rx, uint64_t *tx)
{
	FILE *f;
	int rc;

	*id = if_nametoindex(ifname);
	if (*id == 0 || !(f = open_proc("net/dev")))
		return -1;
	rc = lp_parse_netdev(f, ifname, rx, tx);
	fclose(f);
	return rc;
}

enum lp_route_result lp_os_default_interface(char name[LP_IFNAME_SIZE])
{
	enum lp_route_result r = LP_ROUTE_NONE;
	FILE *f;

	if ((f = open_proc("net/route"))) {
		r = lp_route_v4(f, name);
		fclose(f);
	}
	if (r == LP_ROUTE_NONE && (f = open_proc("net/ipv6_route"))) {
		r = lp_route_v6(f, name);
		fclose(f);
	}
	return r;
}
