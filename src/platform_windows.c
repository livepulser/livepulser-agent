#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"

#define WPATH_SIZE 1100
#define UNIX_EPOCH_FILETIME 116444736000000000ULL

struct lp_dir {
	wchar_t path[WPATH_SIZE];
	HANDLE lock;
};

static HANDLE wake_event;
static volatile LONG stop_flag;
static volatile LONG reload_flag;

const char *lp_os_name(void)
{
	return "windows";
}

const char *lp_arch(void)
{
	SYSTEM_INFO si;

	GetNativeSystemInfo(&si);
	switch (si.wProcessorArchitecture) {
	case PROCESSOR_ARCHITECTURE_AMD64:
		return "amd64";
	case PROCESSOR_ARCHITECTURE_ARM64:
		return "arm64";
	case PROCESSOR_ARCHITECTURE_INTEL:
		return "386";
	default:
		return "unknown";
	}
}

static const char *program_data(void)
{
	const char *p = getenv("ProgramData");
	return p && *p ? p : "C:\\ProgramData";
}

const char *lp_default_config_path(void)
{
	static char path[LP_FILE_PATH_SIZE];

	if (!path[0])
		snprintf(path, sizeof path, "%s\\LivePulser\\agent.conf", program_data());
	return path;
}

const char *lp_default_state_dir(void)
{
	static char path[LP_FILE_PATH_SIZE];

	if (!path[0])
		snprintf(path, sizeof path, "%s\\LivePulser\\state", program_data());
	return path;
}

static bool drive_path(const char *p)
{
	return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

bool lp_path_is_absolute(const char *path)
{
	return drive_path(path) || (path[0] == '\\' && path[1] == '\\');
}

/* "/" means the system drive, which is also the server-side default. */
bool lp_valid_disk_path(const char *path)
{
	return strcmp(path, "/") == 0 || drive_path(path);
}

bool lp_valid_ifname(const char *name)
{
	size_t n = strlen(name);

	if (n == 0 || n >= LP_IFNAME_SIZE)
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c == 0x7f)
			return false;
	}
	return true;
}

/* The measurement clock excludes sleep; the tick count includes it. */
int64_t lp_mono_ms(void)
{
	ULONGLONG t;

	QueryUnbiasedInterruptTime(&t);
	return (int64_t)(t / 10000);
}

void lp_clock_now(struct lp_clock *t)
{
	FILETIME ft;
	ULARGE_INTEGER u;

	t->mono_ms = lp_mono_ms();
	t->boot_ms = (int64_t)GetTickCount64();
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	t->real_ms = (int64_t)((u.QuadPart - UNIX_EPOCH_FILETIME) / 10000);
}

bool lp_gmtime(int64_t unix_s, struct tm *out)
{
	__time64_t t = (__time64_t)unix_s;

	return _gmtime64_s(out, &t) == 0;
}

int lp_random_bytes(void *buf, size_t n)
{
	return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

static BOOL WINAPI on_console(DWORD type)
{
	(void)type;
	lp_request_stop();
	return TRUE;
}

int lp_events_init(void)
{
	if (!wake_event && !(wake_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
		return -1;
	SetConsoleCtrlHandler(on_console, TRUE);
	return 0;
}

void lp_wait_until(int64_t deadline_ms)
{
	DWORD timeout = INFINITE;

	if (deadline_ms != LP_NEVER) {
		int64_t left = deadline_ms - lp_mono_ms();
		timeout = left <= 0 ? 0 : left >= INFINITE ? INFINITE - 1 : (DWORD)left;
	}
	WaitForSingleObject(wake_event, timeout);
}

bool lp_stop_requested(void)
{
	return InterlockedCompareExchange(&stop_flag, 0, 0) != 0;
}

bool lp_take_reload(void)
{
	return InterlockedExchange(&reload_flag, 0) != 0;
}

void lp_request_stop(void)
{
	InterlockedExchange(&stop_flag, 1);
	if (wake_event)
		SetEvent(wake_event);
}

void lp_request_reload(void)
{
	InterlockedExchange(&reload_flag, 1);
	if (wake_event)
		SetEvent(wake_event);
}

static bool widen(const char *s, wchar_t *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}

static bool join(const wchar_t *dir, const char *name, wchar_t *out, size_t cap)
{
	wchar_t wname[64];
	int n;

	if (!widen(name, wname, 64))
		return false;
	n = _snwprintf(out, cap, L"%ls\\%ls", dir, wname);
	return n > 0 && (size_t)n < cap;
}

/* Rejects files whose DACL lets everyone, authenticated users or the Users group read them. */
static bool readable_by_others(HANDLE h)
{
	static const WELL_KNOWN_SID_TYPE broad[] = {WinWorldSid, WinAuthenticatedUserSid, WinBuiltinUsersSid, WinAnonymousSid};
	PSECURITY_DESCRIPTOR sd = NULL;
	PACL dacl = NULL;
	bool open = false;

	if (GetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
		return true;
	if (!dacl)
		open = true;
	for (DWORD i = 0; dacl && !open && i < dacl->AceCount; i++) {
		ACE_HEADER *ace;
		if (!GetAce(dacl, i, (void **)&ace) || ace->AceType != ACCESS_ALLOWED_ACE_TYPE)
			continue;
		ACCESS_ALLOWED_ACE *allow = (ACCESS_ALLOWED_ACE *)ace;
		if (!(allow->Mask & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)))
			continue;
		for (size_t k = 0; k < sizeof broad / sizeof broad[0]; k++) {
			BYTE sid[SECURITY_MAX_SID_SIZE];
			DWORD size = sizeof sid;
			if (CreateWellKnownSid(broad[k], NULL, sid, &size) && EqualSid(sid, (PSID)&allow->SidStart))
				open = true;
		}
	}
	LocalFree(sd);
	return open;
}

static int read_handle(HANDLE h, const char *label, bool secret, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	LARGE_INTEGER size;
	DWORD got = 0;

	if (GetFileType(h) != FILE_TYPE_DISK || !GetFileSizeEx(h, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart >= cap) {
		snprintf(err, errlen, "%s must be a regular file smaller than %zu bytes", label, cap);
		return -1;
	}
	if (secret && readable_by_others(h)) {
		snprintf(err, errlen, "%s must only be readable by administrators and the agent service", label);
		return -1;
	}
	if (!ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL)) {
		snprintf(err, errlen, "cannot read %s (error %lu)", label, GetLastError());
		return -1;
	}
	buf[got] = '\0';
	*len = got;
	return 0;
}

static int read_path(const char *path, bool secret, char *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
	wchar_t wpath[WPATH_SIZE];
	HANDLE h;
	int rc;

	if (!widen(path, wpath, WPATH_SIZE)) {
		snprintf(err, errlen, "invalid path %s", path);
		return -1;
	}
	h = CreateFileW(wpath, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		snprintf(err, errlen, "cannot open %s (error %lu)", path, GetLastError());
		return -1;
	}
	rc = read_handle(h, path, secret, buf, cap, len, err, errlen);
	CloseHandle(h);
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
	wchar_t lock[WPATH_SIZE];
	OVERLAPPED ov = {0};
	DWORD attrs;

	if (!d) {
		snprintf(err, errlen, "out of memory");
		return LP_DIR_ERROR;
	}
	d->lock = INVALID_HANDLE_VALUE;
	if (!widen(path, d->path, WPATH_SIZE) || (attrs = GetFileAttributesW(d->path)) == INVALID_FILE_ATTRIBUTES ||
	    !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		snprintf(err, errlen, "cannot open state directory %s", path);
		goto fail;
	}
	if (!join(d->path, "agent.lock", lock, WPATH_SIZE) ||
	    (d->lock = CreateFileW(lock, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
				   FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE) {
		snprintf(err, errlen, "cannot open lock file (error %lu)", GetLastError());
		goto fail;
	}
	if (!LockFileEx(d->lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
		bool busy = GetLastError() == ERROR_LOCK_VIOLATION;
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
	wchar_t path[WPATH_SIZE];
	char err[64];
	HANDLE h;
	int rc;

	if (!join(d->path, name, path, WPATH_SIZE))
		return LP_DIR_ERROR;
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND ? LP_DIR_MISSING : LP_DIR_ERROR;
	rc = read_handle(h, name, false, buf, cap, len, err, sizeof err);
	CloseHandle(h);
	return rc == 0 ? LP_DIR_OK : LP_DIR_ERROR;
}

int lp_dir_write_atomic(struct lp_dir *d, const char *name, const char *data, size_t len, char *err, size_t errlen)
{
	wchar_t path[WPATH_SIZE], tmp[WPATH_SIZE];
	char tmpname[64];
	DWORD written = 0;
	HANDLE h;
	bool ok;

	snprintf(tmpname, sizeof tmpname, "%s.tmp", name);
	if (!join(d->path, name, path, WPATH_SIZE) || !join(d->path, tmpname, tmp, WPATH_SIZE)) {
		snprintf(err, errlen, "state path too long");
		return -1;
	}
	h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		snprintf(err, errlen, "cannot write state (error %lu)", GetLastError());
		return -1;
	}
	ok = WriteFile(h, data, (DWORD)len, &written, NULL) && written == len && FlushFileBuffers(h);
	CloseHandle(h);
	if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		snprintf(err, errlen, "cannot persist state (error %lu)", GetLastError());
		DeleteFileW(tmp);
		return -1;
	}
	return 0;
}

int lp_dir_rename(struct lp_dir *d, const char *from, const char *to)
{
	wchar_t a[WPATH_SIZE], b[WPATH_SIZE];

	if (!join(d->path, from, a, WPATH_SIZE) || !join(d->path, to, b, WPATH_SIZE))
		return LP_DIR_ERROR;
	if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		return LP_DIR_OK;
	return GetLastError() == ERROR_FILE_NOT_FOUND ? LP_DIR_MISSING : LP_DIR_ERROR;
}

void lp_dir_close(struct lp_dir *d)
{
	if (!d)
		return;
	if (d->lock != INVALID_HANDLE_VALUE)
		CloseHandle(d->lock);
	free(d);
}
