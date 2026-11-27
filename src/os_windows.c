#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collector.h"

static PDH_HQUERY pdh_query;
static PDH_HCOUNTER pdh_swap;
static bool swap_ready;

int lp_os_init(void)
{
	if (PdhOpenQueryW(NULL, 0, &pdh_query) != ERROR_SUCCESS)
		return -1;
	swap_ready = PdhAddEnglishCounterW(pdh_query, L"\\Paging File(_Total)\\% Usage", 0, &pdh_swap) == ERROR_SUCCESS;
	return 0;
}

void lp_os_cleanup(void)
{
	if (pdh_query)
		PdhCloseQuery(pdh_query);
	pdh_query = NULL;
	swap_ready = false;
}

static uint64_t filetime(FILETIME ft)
{
	return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* Kernel time already includes idle time. */
int lp_os_cpu(struct lp_cpu *out)
{
	FILETIME idle, kernel, user;

	memset(out, 0, sizeof *out);
	if (!GetSystemTimes(&idle, &kernel, &user))
		return -1;
	out->field[0] = filetime(user);
	out->field[1] = filetime(kernel);
	out->idle = filetime(idle);
	out->ncpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	return lp_cpu_finish(out);
}

int lp_os_memory(double *out)
{
	MEMORYSTATUSEX ms = {.dwLength = sizeof ms};

	if (!GlobalMemoryStatusEx(&ms))
		return -1;
	return lp_used_percent(ms.ullTotalPhys, ms.ullAvailPhys, out);
}

int lp_os_swap(double *out)
{
	PDH_FMT_COUNTERVALUE v;

	if (!swap_ready || PdhCollectQueryData(pdh_query) != ERROR_SUCCESS ||
	    PdhGetFormattedCounterValue(pdh_swap, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &v) != ERROR_SUCCESS ||
	    v.CStatus != PDH_CSTATUS_VALID_DATA || v.doubleValue < 0 || v.doubleValue > 100)
		return -1;
	*out = v.doubleValue;
	return 0;
}

int lp_os_disk(const char *path, double *out)
{
	char root[LP_DISK_PATH_SIZE + 4];
	wchar_t wpath[LP_DISK_PATH_SIZE + 4];
	ULARGE_INTEGER total, free_bytes;
	const char *drive;

	if (strcmp(path, "/") == 0) {
		drive = getenv("SystemDrive");
		snprintf(root, sizeof root, "%s\\", drive && *drive ? drive : "C:");
	} else {
		snprintf(root, sizeof root, "%s", path);
		for (char *p = root; *p; p++) {
			if (*p == '/')
				*p = '\\';
		}
	}
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, wpath, (int)(sizeof wpath / sizeof wpath[0])) ||
	    !GetDiskFreeSpaceExW(wpath, NULL, &total, &free_bytes))
		return -1;
	return lp_used_percent(total.QuadPart, free_bytes.QuadPart, out);
}

static bool interface_row(const NET_LUID *luid, MIB_IF_ROW2 *row)
{
	memset(row, 0, sizeof *row);
	row->InterfaceLuid = *luid;
	return GetIfEntry2(row) == NO_ERROR;
}

int lp_os_net(const char *ifname, uint64_t *id, uint64_t *rx, uint64_t *tx)
{
	wchar_t alias[IF_MAX_STRING_SIZE + 1];
	MIB_IF_ROW2 row;
	NET_LUID luid;

	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ifname, -1, alias, IF_MAX_STRING_SIZE + 1) ||
	    ConvertInterfaceAliasToLuid(alias, &luid) != NO_ERROR || !interface_row(&luid, &row))
		return -1;
	*id = luid.Value;
	*rx = row.InOctets;
	*tx = row.OutOctets;
	return 0;
}

/* Effective route metric is the route metric plus the interface metric, as used by Windows itself. */
static enum lp_route_result default_route(ADDRESS_FAMILY family, char name[LP_IFNAME_SIZE])
{
	PMIB_IPFORWARD_TABLE2 table;
	NET_LUID best = {0};
	uint64_t best_metric = UINT64_MAX;
	bool ambiguous = false;
	wchar_t alias[IF_MAX_STRING_SIZE + 1];

	if (GetIpForwardTable2(family, &table) != NO_ERROR)
		return LP_ROUTE_NONE;
	for (ULONG i = 0; i < table->NumEntries; i++) {
		const MIB_IPFORWARD_ROW2 *r = &table->Table[i];
		MIB_IPINTERFACE_ROW ir = {0};
		MIB_IF_ROW2 row;
		uint64_t metric;

		if (r->DestinationPrefix.PrefixLength != 0)
			continue;
		ir.Family = family;
		ir.InterfaceLuid = r->InterfaceLuid;
		if (GetIpInterfaceEntry(&ir) != NO_ERROR || !interface_row(&r->InterfaceLuid, &row) ||
		    row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp)
			continue;
		metric = (uint64_t)r->Metric + ir.Metric;
		if (metric < best_metric) {
			best_metric = metric;
			best = r->InterfaceLuid;
			ambiguous = false;
		} else if (metric == best_metric && r->InterfaceLuid.Value != best.Value) {
			ambiguous = true;
		}
	}
	FreeMibTable(table);
	if (best_metric == UINT64_MAX)
		return LP_ROUTE_NONE;
	if (ambiguous)
		return LP_ROUTE_AMBIGUOUS;
	if (ConvertInterfaceLuidToAlias(&best, alias, IF_MAX_STRING_SIZE + 1) != NO_ERROR ||
	    !WideCharToMultiByte(CP_UTF8, 0, alias, -1, name, LP_IFNAME_SIZE, NULL, NULL))
		return LP_ROUTE_NONE;
	return LP_ROUTE_FOUND;
}

enum lp_route_result lp_os_default_interface(char name[LP_IFNAME_SIZE])
{
	enum lp_route_result r = default_route(AF_INET, name);

	return r == LP_ROUTE_NONE ? default_route(AF_INET6, name) : r;
}
