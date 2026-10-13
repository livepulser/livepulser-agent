#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "transport.h"

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

#define WIDE_URL_SIZE 160
#define HEADER_SIZE 512

struct lp_transport {
	HINTERNET session;
	HINTERNET connect;
	bool secure;
	wchar_t config_path[WIDE_URL_SIZE];
	wchar_t telemetry_path[WIDE_URL_SIZE];
	char key[LP_API_KEY_LEN + 1];
	char body[LP_MAX_CONFIG_BYTES + 1];
};

static void describe_error(struct lp_response *r, DWORD code)
{
	char msg[160] = "";
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, GetModuleHandleW(L"winhttp.dll"),
				 code, 0, msg, sizeof msg, NULL);

	while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' '))
		msg[--n] = '\0';
	r->transport_error = true;
	r->tls_error = code == ERROR_WINHTTP_SECURE_FAILURE || (code >= ERROR_WINHTTP_SECURE_CERT_DATE_INVALID &&
							      code <= ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE) ||
		       code == ERROR_WINHTTP_SECURE_CHANNEL_ERROR || code == ERROR_WINHTTP_SECURE_INVALID_CERT ||
		       code == ERROR_WINHTTP_SECURE_CERT_REVOKED || code == ERROR_WINHTTP_SECURE_INVALID_CA;
	snprintf(r->error, sizeof r->error, "WinHTTP error %lu%s%.200s", code, n ? ": " : "", msg);
}

static void query_header(HINTERNET req, DWORD which, char *out, size_t cap)
{
	wchar_t value[LP_ETAG_SIZE];
	DWORD size = sizeof value;

	out[0] = '\0';
	if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, value, &size, WINHTTP_NO_HEADER_INDEX))
		return;
	for (size_t i = 0; value[i]; i++) {
		if (value[i] < 0x20 || value[i] >= 0x7f || i + 1 >= cap) {
			out[0] = '\0';
			return;
		}
		out[i] = (char)value[i];
		out[i + 1] = '\0';
	}
}

static int perform(struct lp_transport *t, const wchar_t *verb, const wchar_t *path, const char *extra,
		   const char *body, size_t len, struct lp_response *r)
{
	char headers[HEADER_SIZE], value[LP_ETAG_SIZE];
	wchar_t wheaders[HEADER_SIZE];
	HINTERNET req;
	DWORD status = 0, size = sizeof status, got;
	int rc = -1;

	memset(r, 0, sizeof *r);
	r->retry_after_ms = -1;
	r->body = t->body;
	t->body[0] = '\0';
	snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n%s", t->key, extra);
	MultiByteToWideChar(CP_UTF8, 0, headers, -1, wheaders, HEADER_SIZE);
	SecureZeroMemory(headers, sizeof headers);

	req = WinHttpOpenRequest(t->connect, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
				 t->secure ? WINHTTP_FLAG_SECURE : 0);
	if (!req) {
		describe_error(r, GetLastError());
		goto out;
	}
	DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
	WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable);
	if (!WinHttpSendRequest(req, wheaders, (DWORD)-1L, (LPVOID)body, (DWORD)len, (DWORD)len, 0) ||
	    !WinHttpReceiveResponse(req, NULL)) {
		describe_error(r, GetLastError());
		goto out;
	}
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			    &status, &size, WINHTTP_NO_HEADER_INDEX);
	r->status = (long)status;
	query_header(req, WINHTTP_QUERY_ETAG, r->etag, sizeof r->etag);
	query_header(req, WINHTTP_QUERY_RETRY_AFTER, value, sizeof value);
	if (value[0]) {
		int64_t secs = lp_parse_retry_after(value, (int64_t)time(NULL));
		r->retry_after_ms = secs < 0 ? -1 : lp_seconds(secs);
	}
	query_header(req, WINHTTP_QUERY_DATE, value, sizeof value);
	if (value[0]) {
		int64_t d = lp_parse_http_date(value);
		r->server_date = d < 0 ? 0 : d;
	}
	for (;;) {
		if (r->body_len >= LP_MAX_CONFIG_BYTES) {
			char probe;
			if (WinHttpReadData(req, &probe, 1, &got) && got > 0)
				r->body_overflow = true;
			break;
		}
		if (!WinHttpReadData(req, t->body + r->body_len, (DWORD)(LP_MAX_CONFIG_BYTES - r->body_len), &got)) {
			describe_error(r, GetLastError());
			goto out;
		}
		if (got == 0)
			break;
		r->body_len += got;
	}
	t->body[r->body_len] = '\0';
	rc = 0;
out:
	SecureZeroMemory(wheaders, sizeof wheaders);
	if (req)
		WinHttpCloseHandle(req);
	return rc;
}

int lp_fetch_config(struct lp_transport *t, const char *etag, struct lp_response *r)
{
	char extra[LP_ETAG_SIZE + 64];

	if (etag && etag[0])
		snprintf(extra, sizeof extra, "Accept: application/json\r\nIf-None-Match: %s\r\n", etag);
	else
		snprintf(extra, sizeof extra, "Accept: application/json\r\n");
	return perform(t, L"GET", t->config_path, extra, NULL, 0, r);
}

int lp_send_report(struct lp_transport *t, const char *body, size_t len, struct lp_response *r)
{
	return perform(t, L"POST", t->telemetry_path, "Content-Type: application/json\r\n", body, len, r);
}

void lp_transport_set_key(struct lp_transport *t, const char *api_key)
{
	snprintf(t->key, sizeof t->key, "%s", api_key);
}

static bool split_endpoint(const char *endpoint, bool *secure, wchar_t *host, size_t hostcap, INTERNET_PORT *port)
{
	char h[LP_ENDPOINT_SIZE];
	const char *p, *colon;

	*secure = strncmp(endpoint, "https://", 8) == 0;
	p = endpoint + (*secure ? 8 : 7);
	*port = *secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
	colon = p[0] == '[' ? strstr(p, "]:") : strrchr(p, ':');
	if (colon && p[0] == '[')
		colon++;
	snprintf(h, sizeof h, "%.*s", colon ? (int)(colon - p) : (int)strlen(p), p);
	if (colon) {
		long v = strtol(colon + 1, NULL, 10);
		if (v < 1 || v > 65535)
			return false;
		*port = (INTERNET_PORT)v;
	}
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, h, -1, host, (int)hostcap) > 0;
}

struct lp_transport *lp_transport_new(const struct lp_options *o, const char *api_key, char *err, size_t errlen)
{
	struct lp_transport *t = calloc(1, sizeof *t);
	wchar_t host[LP_ENDPOINT_SIZE], proxy[LP_ENDPOINT_SIZE], ua[64];
	INTERNET_PORT port;
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;

	if (!t || !split_endpoint(o->endpoint, &t->secure, host, LP_ENDPOINT_SIZE, &port)) {
		snprintf(err, errlen, "cannot prepare HTTP client for %s", o->endpoint);
		free(t);
		return NULL;
	}
	MultiByteToWideChar(CP_UTF8, 0, LP_AGENT_NAME "/" LP_VERSION, -1, ua, 64);
	if (o->proxy[0]) {
		const char *p = strstr(o->proxy, "://");
		MultiByteToWideChar(CP_UTF8, 0, p ? p + 3 : o->proxy, -1, proxy, LP_ENDPOINT_SIZE);
		t->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy, WINHTTP_NO_PROXY_BYPASS, 0);
	} else {
		/* Like the POSIX build, ignore system-wide proxy settings unless one is configured explicitly. */
		t->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (!t->session) {
		snprintf(err, errlen, "WinHttpOpen failed (error %lu)", GetLastError());
		free(t);
		return NULL;
	}
	WinHttpSetTimeouts(t->session, 3000, 3000, 10000, 10000);
	WinHttpSetOption(t->session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
	if (!WinHttpSetOption(t->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(t->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
	}
	t->connect = WinHttpConnect(t->session, host, port, 0);
	if (!t->connect) {
		snprintf(err, errlen, "WinHttpConnect failed (error %lu)", GetLastError());
		WinHttpCloseHandle(t->session);
		free(t);
		return NULL;
	}
	_snwprintf(t->config_path, WIDE_URL_SIZE, L"/v1/resource/%hs/config", o->monitor_id);
	_snwprintf(t->telemetry_path, WIDE_URL_SIZE, L"/v1/resource/%hs/telemetry", o->monitor_id);
	lp_transport_set_key(t, api_key);
	return t;
}

void lp_transport_free(struct lp_transport *t)
{
	if (!t)
		return;
	WinHttpCloseHandle(t->connect);
	WinHttpCloseHandle(t->session);
	SecureZeroMemory(t->key, sizeof t->key);
	free(t);
}
