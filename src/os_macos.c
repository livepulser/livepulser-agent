#include <mach/mach.h>
#include <stdio.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include "collector.h"
#include "platform.h"



static mach_port_t host;

int lp_os_init(void)
{
	host = mach_host_self();
	return 0;
}

void lp_os_cleanup(void)
{
	mach_port_deallocate(mach_task_self(), host);
}

/* Per-CPU tick counters are 32-bit; a wrap shows up as a decrease and invalidates that window. */
int lp_os_cpu(struct lp_cpu *out)
{
	processor_info_array_t info;
	mach_msg_type_number_t count;
	natural_t ncpu;

	memset(out, 0, sizeof *out);
	if (host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &count) != KERN_SUCCESS)
		return -1;
	for (natural_t i = 0; i < ncpu; i++) {
		const integer_t *t = info + (size_t)i * CPU_STATE_MAX;
		out->field[0] += (uint32_t)t[CPU_STATE_USER];
		out->field[1] += (uint32_t)t[CPU_STATE_NICE];
		out->field[2] += (uint32_t)t[CPU_STATE_SYSTEM];
		out->field[3] += (uint32_t)t[CPU_STATE_IDLE];
	}
	vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)count * sizeof(integer_t));
	out->ncpu = (int)ncpu;
	out->idle = out->field[3];
	return lp_cpu_finish(out);
}

/* Matches Activity Monitor's "Memory Used": app memory + wired + compressed. */
int lp_os_memory(double *out)
{
	vm_statistics64_data_t vm;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	uint64_t total, used;
	size_t len = sizeof total;
	vm_size_t page;

	if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) != 0 || host_page_size(host, &page) != KERN_SUCCESS ||
	    host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS)
		return -1;
	if (vm.internal_page_count < vm.purgeable_count)
		return -1;
	used = ((uint64_t)vm.internal_page_count - vm.purgeable_count + vm.wire_count + vm.compressor_page_count) * page;
	if (used > total)
		return -1;
	return lp_used_percent(total, total - used, out);
}

/* macOS creates swap files on demand, so an empty swap area means 0% rather than unavailable. */
int lp_os_swap(double *out)
{
	struct xsw_usage sw;
	size_t len = sizeof sw;

	if (sysctlbyname("vm.swapusage", &sw, &len, NULL, 0) != 0)
		return -1;
	if (sw.xsu_total == 0) {
		*out = 0;
		return 0;
	}
	return lp_used_percent(sw.xsu_total, sw.xsu_avail, out);
}

int lp_os_disk(const char *path, double *out)
{
	struct statfs fs;

	if (statfs(path, &fs) != 0)
		return -1;
	return lp_used_percent(fs.f_blocks, fs.f_bfree, out);
}

int lp_os_net(const char *ifname, uint64_t *id, uint64_t *rx, uint64_t *tx)
{
	unsigned index = if_nametoindex(ifname);
	int mib[6] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, (int)index};
	char buf[2048];
	size_t len = sizeof buf;

	if (index == 0 || sysctl(mib, 6, buf, &len, NULL, 0) != 0)
		return -1;
	for (size_t off = 0; off + sizeof(struct if_msghdr) <= len;) {
		const struct if_msghdr *ifm = (const struct if_msghdr *)(buf + off);
		if (ifm->ifm_msglen == 0 || off + ifm->ifm_msglen > len)
			break;
		if (ifm->ifm_type == RTM_IFINFO2 && ifm->ifm_index == index && ifm->ifm_msglen >= sizeof(struct if_msghdr2)) {
			const struct if_msghdr2 *m2 = (const struct if_msghdr2 *)ifm;
			*id = index;
			*rx = m2->ifm_data.ifi_ibytes;
			*tx = m2->ifm_data.ifi_obytes;
			return 0;
		}
		off += ifm->ifm_msglen;
	}
	return -1;
}

static bool zero_address(const struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET)
		return ((const struct sockaddr_in *)(const void *)sa)->sin_addr.s_addr == 0;
	if (sa->sa_family == AF_INET6)
		return IN6_IS_ADDR_UNSPECIFIED(&((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr);
	return false;
}

/*
 * The primary default route is the unscoped one; interface-scoped defaults (RTF_IFSCOPE)
 * exist for every additional network service and are not used for ordinary traffic.
 */
static enum lp_route_result default_route(int family, char name[LP_IFNAME_SIZE])
{
	int mib[6] = {CTL_NET, PF_ROUTE, 0, family, NET_RT_FLAGS, RTF_GATEWAY};
	unsigned found = 0;
	bool ambiguous = false;
	size_t len = 0;
	char *buf;

	if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0 || len > 4 * 1024 * 1024)
		return LP_ROUTE_NONE;
	len += len / 8;
	if (!(buf = malloc(len)))
		return LP_ROUTE_NONE;
	if (sysctl(mib, 6, buf, &len, NULL, 0) != 0)
		len = 0;
	for (size_t off = 0; off + sizeof(struct rt_msghdr) <= len;) {
		const struct rt_msghdr *rtm = (const struct rt_msghdr *)(buf + off);
		const struct sockaddr *dst = (const struct sockaddr *)(rtm + 1);
		if (rtm->rtm_msglen == 0 || off + rtm->rtm_msglen > len)
			break;
		off += rtm->rtm_msglen;
		if (!(rtm->rtm_addrs & RTA_DST) || (rtm->rtm_flags & (RTF_HOST | RTF_IFSCOPE | RTF_REJECT)) ||
		    !(rtm->rtm_flags & RTF_UP) || (const char *)dst + sizeof(struct sockaddr) > (const char *)rtm + rtm->rtm_msglen)
			continue;
		if (dst->sa_family != family || !zero_address(dst) || rtm->rtm_index == 0)
			continue;
		if (found && found != rtm->rtm_index)
			ambiguous = true;
		found = rtm->rtm_index;
	}
	free(buf);
	if (!found)
		return LP_ROUTE_NONE;
	if (ambiguous)
		return LP_ROUTE_AMBIGUOUS;
	char ifname[IF_NAMESIZE];
	if (!if_indextoname(found, ifname) || strcmp(ifname, "lo0") == 0)
		return LP_ROUTE_NONE;
	snprintf(name, LP_IFNAME_SIZE, "%s", ifname);
	return LP_ROUTE_FOUND;
}

enum lp_route_result lp_os_default_interface(char name[LP_IFNAME_SIZE])
{
	enum lp_route_result r = default_route(AF_INET, name);

	return r == LP_ROUTE_NONE ? default_route(AF_INET6, name) : r;
}
