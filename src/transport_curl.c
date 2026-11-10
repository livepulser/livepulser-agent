#include "transport.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_HEADER_VALUE 128

struct lp_transport {
	CURL *curl;
	char config_url[LP_ENDPOINT_SIZE + 64];
	char telemetry_url[LP_ENDPOINT_SIZE + 64];
	char auth[96];
	char errbuf[CURL_ERROR_SIZE];
	struct lp_response *response;
	char body[LP_MAX_CONFIG_BYTES + 1];
	size_t body_len;
	bool overflow;
};

static size_t on_body(char *data, size_t size, size_t nmemb, void *userdata)
{
	struct lp_transport *t = userdata;
	size_t n = size * nmemb;

	if (n > LP_MAX_CONFIG_BYTES - t->body_len) {
		t->overflow = true;
		return 0;
	}
	memcpy(t->body + t->body_len, data, n);
	t->body_len += n;
	return n;
}

static bool header_value(const char *line, size_t len, const char *name, char *out, size_t cap)
{
	size_t n = strlen(name), v;

	if (len <= n || strncasecmp(line, name, n) != 0 || line[n] != ':')
		return false;
	line += n + 1;
	len -= n + 1;
	while (len && (*line == ' ' || *line == '\t')) {
		line++;
		len--;
	}
	while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' '))
		len--;
	if (len >= cap)
		return false;
	for (v = 0; v < len; v++) {
		if ((unsigned char)line[v] < 0x20 || (unsigned char)line[v] >= 0x7f)
			return false;
	}
	memcpy(out, line, len);
	out[len] = '\0';
	return true;
}

static size_t on_header(char *line, size_t size, size_t nmemb, void *userdata)
{
	struct lp_transport *t = userdata;
	struct lp_response *r = t->response;
	size_t len = size * nmemb;
	char value[MAX_HEADER_VALUE];

	if (len >= 5 && strncmp(line, "HTTP/", 5) == 0) {
		r->etag[0] = '\0';
		r->retry_after_ms = -1;
		r->server_date = 0;
		r->config_version = 0;
		r->conflict[0] = '\0';
	} else if (header_value(line, len, "LP-Config-Version", value, sizeof value)) {
		r->config_version = strtoll(value, NULL, 10);
	} else if (header_value(line, len, "LP-Conflict", value, sizeof value)) {
		snprintf(r->conflict, sizeof r->conflict, "%.15s", value);
	} else if (header_value(line, len, "ETag", value, sizeof value)) {
		snprintf(r->etag, sizeof r->etag, "%s", value);
	} else if (header_value(line, len, "Retry-After", value, sizeof value)) {
		int64_t secs = lp_parse_retry_after(value, (int64_t)time(NULL));
		r->retry_after_ms = secs < 0 ? -1 : lp_seconds(secs);
	} else if (header_value(line, len, "Date", value, sizeof value)) {
		int64_t d = lp_parse_http_date(value);
		r->server_date = d < 0 ? 0 : d;
	}
	return len;
}

static void wipe_headers(struct curl_slist *list)
{
	for (struct curl_slist *h = list; h; h = h->next)
		lp_wipe(h->data, strlen(h->data));
	curl_slist_free_all(list);
}

static bool is_tls_error(CURLcode c)
{
	return c == CURLE_PEER_FAILED_VERIFICATION || c == CURLE_SSL_CONNECT_ERROR ||
	       c == CURLE_SSL_CACERT_BADFILE || c == CURLE_SSL_CERTPROBLEM || c == CURLE_SSL_CIPHER;
}

static int perform(struct lp_transport *t, const char *url, struct curl_slist *headers, const char *body, size_t len, struct lp_response *r)
{
	CURLcode c;

	memset(r, 0, sizeof *r);
	r->retry_after_ms = -1;
	t->response = r;
	t->body_len = 0;
	t->overflow = false;
	t->errbuf[0] = '\0';

	curl_easy_setopt(t->curl, CURLOPT_URL, url);
	curl_easy_setopt(t->curl, CURLOPT_HTTPHEADER, headers);
	if (body) {
		curl_easy_setopt(t->curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(t->curl, CURLOPT_POSTFIELDSIZE, (long)len);
	} else {
		curl_easy_setopt(t->curl, CURLOPT_HTTPGET, 1L);
	}
	c = curl_easy_perform(t->curl);
	curl_easy_setopt(t->curl, CURLOPT_HTTPHEADER, NULL);
	wipe_headers(headers);

	t->body[t->body_len] = '\0';
	r->body = t->body;
	r->body_len = t->body_len;
	r->body_overflow = t->overflow;
	if (c != CURLE_OK) {
		r->transport_error = true;
		r->tls_error = is_tls_error(c);
		snprintf(r->error, sizeof r->error, "%.48s%s%.200s", curl_easy_strerror(c), t->errbuf[0] ? ": " : "", t->errbuf);
		return -1;
	}
	curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &r->status);
	return 0;
}

static struct curl_slist *auth_headers(struct lp_transport *t)
{
	return curl_slist_append(NULL, t->auth);
}

int lp_fetch_config(struct lp_transport *t, const char *etag, struct lp_response *r)
{
	struct curl_slist *h = auth_headers(t), *next;
	char inm[LP_ETAG_SIZE + 32];

	next = h ? curl_slist_append(h, "Accept: application/json") : NULL;
	if (next && etag && etag[0]) {
		snprintf(inm, sizeof inm, "If-None-Match: %s", etag);
		next = curl_slist_append(next, inm);
	}
	if (!next) {
		wipe_headers(h);
		memset(r, 0, sizeof *r);
		r->transport_error = true;
		snprintf(r->error, sizeof r->error, "out of memory");
		return -1;
	}
	return perform(t, t->config_url, next, NULL, 0, r);
}

int lp_send_report(struct lp_transport *t, const char *body, size_t len, struct lp_response *r)
{
	struct curl_slist *h = auth_headers(t), *next;

	next = h ? curl_slist_append(h, "Content-Type: application/json") : NULL;
	next = next ? curl_slist_append(next, "Expect:") : NULL;
	if (!next) {
		wipe_headers(h);
		memset(r, 0, sizeof *r);
		r->transport_error = true;
		snprintf(r->error, sizeof r->error, "out of memory");
		return -1;
	}
	return perform(t, t->telemetry_url, next, body, len, r);
}

void lp_transport_set_key(struct lp_transport *t, const char *api_key)
{
	lp_wipe(t->auth, sizeof t->auth);
	snprintf(t->auth, sizeof t->auth, "Authorization: Bearer %s", api_key);
}

struct lp_transport *lp_transport_new(const struct lp_options *o, const char *api_key, char *err, size_t errlen)
{
	bool https = strncmp(o->endpoint, "https://", 8) == 0;
	struct lp_transport *t;

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		snprintf(err, errlen, "libcurl initialization failed");
		return NULL;
	}
	t = calloc(1, sizeof *t);
	if (!t || !(t->curl = curl_easy_init())) {
		snprintf(err, errlen, "cannot allocate HTTP client");
		free(t);
		curl_global_cleanup();
		return NULL;
	}
	snprintf(t->config_url, sizeof t->config_url, "%s/v1/resource/%s/config", o->endpoint, o->monitor_id);
	snprintf(t->telemetry_url, sizeof t->telemetry_url, "%s/v1/resource/%s/telemetry", o->endpoint, o->monitor_id);
	lp_transport_set_key(t, api_key);

	curl_easy_setopt(t->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(t->curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
	curl_easy_setopt(t->curl, CURLOPT_TIMEOUT_MS, 10000L);
	curl_easy_setopt(t->curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(t->curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(t->curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(t->curl, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
#else
	curl_easy_setopt(t->curl, CURLOPT_PROTOCOLS, https ? CURLPROTO_HTTPS : CURLPROTO_HTTP);
#endif
	/* An empty proxy disables http_proxy/https_proxy from the environment. */
	curl_easy_setopt(t->curl, CURLOPT_PROXY, o->proxy);
	if (o->ca_file[0])
		curl_easy_setopt(t->curl, CURLOPT_CAINFO, o->ca_file);
	curl_easy_setopt(t->curl, CURLOPT_USERAGENT, LP_AGENT_NAME "/" LP_VERSION);
	curl_easy_setopt(t->curl, CURLOPT_MAXCONNECTS, 1L);
	curl_easy_setopt(t->curl, CURLOPT_MAXFILESIZE, (long)LP_MAX_CONFIG_BYTES);
	curl_easy_setopt(t->curl, CURLOPT_ERRORBUFFER, t->errbuf);
	curl_easy_setopt(t->curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(t->curl, CURLOPT_WRITEDATA, t);
	curl_easy_setopt(t->curl, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(t->curl, CURLOPT_HEADERDATA, t);
	return t;
}

void lp_transport_free(struct lp_transport *t)
{
	if (!t)
		return;
	curl_easy_cleanup(t->curl);
	lp_wipe(t->auth, sizeof t->auth);
	free(t);
	curl_global_cleanup();
}
