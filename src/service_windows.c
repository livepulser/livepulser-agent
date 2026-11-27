#include <windows.h>
#include <stdio.h>

#include "agent.h"
#include "platform.h"
#include "service.h"

static SERVICE_STATUS_HANDLE status_handle;
static int (*service_fn)(void *);
static void *service_arg;
static int service_rc = LP_EXIT_RUNTIME;

static void report(DWORD state, int rc)
{
	SERVICE_STATUS st = {0};

	st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	st.dwCurrentState = state;
	st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PARAMCHANGE : 0;
	st.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
	if (state == SERVICE_STOPPED && rc != LP_EXIT_OK) {
		st.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
		st.dwServiceSpecificExitCode = (DWORD)rc;
	}
	SetServiceStatus(status_handle, &st);
}

static DWORD WINAPI handler(DWORD control, DWORD type, LPVOID data, LPVOID ctx)
{
	(void)type;
	(void)data;
	(void)ctx;
	switch (control) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		report(SERVICE_STOP_PENDING, 0);
		lp_request_stop();
		return NO_ERROR;
	case SERVICE_CONTROL_PARAMCHANGE:
		lp_request_reload();
		return NO_ERROR;
	case SERVICE_CONTROL_INTERROGATE:
		return NO_ERROR;
	default:
		return ERROR_CALL_NOT_IMPLEMENTED;
	}
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
	(void)argc;
	(void)argv;
	status_handle = RegisterServiceCtrlHandlerExW(L"" LP_SERVICE_NAME, handler, NULL);
	if (!status_handle)
		return;
	lp_events_init();
	report(SERVICE_RUNNING, 0);
	service_rc = service_fn(service_arg);
	report(SERVICE_STOPPED, service_rc);
}

int lp_service_run(int (*fn)(void *), void *arg)
{
	SERVICE_TABLE_ENTRYW table[] = {{L"" LP_SERVICE_NAME, service_main}, {NULL, NULL}};

	service_fn = fn;
	service_arg = arg;
	if (!StartServiceCtrlDispatcherW(table)) {
		fprintf(stderr, "--service must be started by the Windows service control manager (error %lu)\n", GetLastError());
		return LP_EXIT_CONFIG;
	}
	return service_rc;
}
