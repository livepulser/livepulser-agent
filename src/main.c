#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "collector.h"
#include "config.h"
#include "log.h"
#include "options.h"
#include "platform.h"
#include "policy.h"
#include "protocol.h"
#include "rules.h"
#include "service.h"
#include "state.h"
#include "transport.h"

struct agent {
	struct lp_options opt;
	char key[LP_API_KEY_LEN + 1];
	struct lp_state state;
	struct lp_transport *http;
	struct lp_collector collector;

	bool have_config;
	struct lp_config cfg;
	char etag[LP_ETAG_SIZE];
	int64_t next_config_ms;
	int config_failures;
	struct lp_guard config_guard;

	bool window_open;
	bool force_select;
	struct lp_counters baseline;
	int64_t window_end_ms;

	struct lp_sample batch[LP_MAX_SAMPLES];
	size_t batch_count;
	struct lp_rules rules;
	int64_t next_upload_ms;
	int64_t event_at[4];

	int64_t send_after_ms;
	int send_failures;
	int rejections;
	bool auth_failed;
	int64_t conflict_since_ms;
	bool session_renewed;
	bool reported_once;
	char body[LP_MAX_REPORT_BYTES + 1];
	size_t body_len;
};

static int64_t min64(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

static int64_t max64(int64_t a, int64_t b)
{
	return a > b ? a : b;
}

/* Changes arrive through LP-Config-Version on every upload; polling is only a safety net. */
static void schedule_config(struct agent *a, int64_t now)
{
	int64_t period = a->have_config && !a->cfg.active ? 60 : 3600;

	a->next_config_ms = now + lp_seconds(period) + lp_jitter_ms(lp_seconds(30));
}

static void config_failed(struct agent *a, int64_t now, const char *why)
{
	a->config_failures++;
	a->next_config_ms = now + lp_backoff_ms(a->config_failures);
	lp_log(a->config_failures == 1 ? LP_WARN : LP_DEBUG, "configuration request failed: %s", why);
}

static void apply_config(struct agent *a, const struct lp_config *c, const char *etag, int64_t now)
{
	bool first = !a->have_config;

	if (first) {
		lp_log(LP_INFO, "configuration loaded: version %lld, upload every %ds, sample every %ds, %s",
		       (long long)c->config_version, c->interval_s, c->sample_s, c->active ? "active" : "paused");
	} else if (c->config_version != a->cfg.config_version) {
		lp_log(LP_INFO, "configuration updated to version %lld", (long long)c->config_version);
	}
	if (!first && a->cfg.active != c->active)
		lp_log(LP_INFO, c->active ? "monitor resumed; sampling again" : "monitor paused; telemetry suspended");
	if (!c->disk_path_valid)
		lp_log(LP_WARN, "configured disk path is not valid on this system; disk usage is unavailable");
	if (first || lp_config_changes_window(&a->cfg, c)) {
		a->window_open = false;
		a->force_select = true;
	}
	if (!c->active) {
		a->batch_count = 0;
		memset(&a->rules, 0, sizeof a->rules);
	}
	if (first || c->interval_s != a->cfg.interval_s || (c->active && !a->cfg.active))
		a->next_upload_ms = now + (int64_t)c->interval_s * 1000;
	a->cfg = *c;
	a->have_config = true;
	snprintf(a->etag, sizeof a->etag, "%s", etag);
	if (first && a->state.recovered_corrupt) {
		a->send_after_ms = now + lp_seconds(c->offline_after_s);
		lp_log(LP_WARN, "local state was corrupt; waiting %ds for the previous session lease to expire", c->offline_after_s);
	}
}

static int refresh_config(struct agent *a, int64_t now)
{
	struct lp_response r;
	struct lp_config c;
	char err[160];
	int64_t allowed = lp_guard_next(&a->config_guard, now);

	if (allowed > now) {
		a->next_config_ms = allowed;
		return 0;
	}
	lp_guard_record(&a->config_guard, now);
	lp_fetch_config(a->http, a->have_config ? a->etag : NULL, &r);
	now = lp_mono_ms();
	if (r.transport_error) {
		config_failed(a, now, r.error);
		return 0;
	}
	switch (r.status) {
	case 200:
		switch (lp_config_parse(r.body, r.body_len, &c, err, sizeof err)) {
		case LP_CONFIG_OK:
			apply_config(a, &c, r.etag, now);
			a->config_failures = 0;
			schedule_config(a, now);
			return 0;
		case LP_CONFIG_UNSUPPORTED:
			lp_log(LP_ERR, "%s; upgrade %s", err, LP_AGENT_NAME);
			return LP_EXIT_CONFIG;
		default:
			config_failed(a, now, err);
			return 0;
		}
	case 304:
		if (!a->have_config) {
			config_failed(a, now, "unexpected 304 response");
			return 0;
		}
		a->config_failures = 0;
		schedule_config(a, now);
		return 0;
	case 401:
		lp_log(LP_ERR, "API key was rejected; replace the key file, then reload or restart the agent");
		if (!a->have_config)
			return LP_EXIT_AUTH;
		a->auth_failed = true;
		a->window_open = false;
		return 0;
	case 429:
		a->next_config_ms = now + max64(r.retry_after_ms, lp_seconds(60)) + lp_jitter_ms(lp_seconds(5));
		lp_log(LP_WARN, "configuration requests are rate limited");
		return 0;
	default:
		snprintf(err, sizeof err, r.status >= 300 && r.status < 400 ? "HTTP %ld redirect refused; check endpoint" : "HTTP %ld", r.status);
		config_failed(a, now, err);
		return 0;
	}
}

static void delay_telemetry(struct agent *a, int64_t now, int64_t server_ms)
{
	a->send_failures++;
	a->send_after_ms = now + max64(lp_backoff_ms(a->send_failures), server_ms);
}

/*
 * A conflict that outlasts the offline lease means the server retired this session (another
 * agent took over and stopped) or holds a newer sequence. Start one new session per process so
 * two hosts sharing a key cannot keep displacing each other.
 */
static void handle_conflict(struct agent *a, const struct lp_response *r, int64_t now)
{
	if (!a->conflict_since_ms) {
		a->conflict_since_ms = now;
		lp_log(LP_WARN, "telemetry conflict (%s); another agent may be using this monitor", r->conflict[0] ? r->conflict : "unspecified");
	}
	if (!a->session_renewed && now - a->conflict_since_ms >= lp_seconds(a->cfg.offline_after_s) + (int64_t)a->cfg.interval_s * 1000) {
		char err[160];
		if (lp_state_new_session(&a->state, err, sizeof err) == 0) {
			a->session_renewed = true;
			a->conflict_since_ms = 0;
			lp_log(LP_WARN, "the server no longer accepts this agent session; starting a new session");
			return;
		}
		lp_log(LP_ERR, "%s", err);
	}
	delay_telemetry(a, now, 0);
}

static void handle_telemetry(struct agent *a, const struct lp_response *r, size_t sent)
{
	int64_t now = lp_mono_ms();

	if (!r->transport_error && r->config_version > 0 && r->config_version != a->cfg.config_version)
		a->next_config_ms = min64(a->next_config_ms, now);
	if (r->transport_error || r->status >= 500) {
		if (r->transport_error)
			lp_log(r->tls_error ? LP_ERR : LP_WARN, "upload failed: %s", r->error);
		else
			lp_log(LP_WARN, "upload not accepted: HTTP %ld", r->status);
		delay_telemetry(a, now, r->retry_after_ms);
		return;
	}
	switch (r->status) {
	case 202:
		if (!a->reported_once || a->send_failures || a->rejections)
			lp_log(LP_INFO, "upload accepted");
		a->reported_once = true;
		a->send_failures = 0;
		a->rejections = 0;
		a->conflict_since_ms = 0;
		memmove(a->batch, a->batch + sent, (a->batch_count - sent) * sizeof a->batch[0]);
		a->batch_count -= sent;
		return;
	case 401:
		lp_log(LP_ERR, "API key was rejected; replace the key file, then reload or restart the agent");
		a->auth_failed = true;
		a->window_open = false;
		a->batch_count = 0;
		return;
	case 403:
		lp_log(LP_INFO, "telemetry refused; the monitor is paused or not provisioned");
		a->next_config_ms = min64(a->next_config_ms, now);
		a->batch_count = 0;
		delay_telemetry(a, now, 0);
		return;
	case 409:
		handle_conflict(a, r, now);
		return;
	case 413:
	case 422:
		a->rejections++;
		if (r->server_date && a->batch_count &&
		    llabs(r->server_date - a->batch[a->batch_count - 1].end_ms / 1000) > 60)
			lp_log(LP_WARN, "upload rejected (HTTP %ld): system clock differs from the server by %llds; check time synchronization",
			       r->status, (long long)(a->batch[a->batch_count - 1].end_ms / 1000 - r->server_date));
		else
			lp_log(LP_WARN, "upload rejected (HTTP %ld): %.200s", r->status, r->body_len ? r->body : "invalid payload");
		a->batch_count = 0;
		if (a->rejections >= 3)
			delay_telemetry(a, now, 0);
		return;
	case 429:
		lp_log(LP_WARN, "telemetry is rate limited");
		a->send_after_ms = now + max64(r->retry_after_ms, lp_seconds(60)) + lp_jitter_ms(lp_seconds(5));
		return;
	default:
		lp_log(LP_WARN, r->status >= 300 && r->status < 400 ? "upload refused: HTTP %ld redirect; check endpoint" : "upload refused: HTTP %ld",
		       r->status);
		delay_telemetry(a, now, 0);
		return;
	}
}

/* Uploads every buffered sample. Failed uploads keep the samples for the next attempt. */
static void upload(struct agent *a, const char *trigger)
{
	struct lp_report rep = {.config_version = a->cfg.config_version, .trigger = trigger, .arch = lp_arch(),
				.samples = a->batch, .count = a->batch_count};
	struct lp_response r;
	int64_t now = lp_mono_ms();
	char err[160];

	if (a->batch_count == 0 || now < a->send_after_ms)
		return;
	if (lp_state_take_sequence(&a->state, &rep.sequence, err, sizeof err) != 0) {
		lp_log(LP_ERR, "%s; not sending until the sequence can be reserved", err);
		delay_telemetry(a, now, 0);
		return;
	}
	memcpy(rep.boot_id, a->state.boot_id, sizeof rep.boot_id);
	if (lp_report_write(&rep, a->body, sizeof a->body, &a->body_len) != 0) {
		lp_log(LP_ERR, "cannot serialize upload; dropping %lu samples", (unsigned long)a->batch_count);
		a->batch_count = 0;
		return;
	}
	lp_log(LP_DEBUG, "uploading %lu samples (%s, sequence %lld)", (unsigned long)a->batch_count, trigger, (long long)rep.sequence);
	lp_send_report(a->http, a->body, a->body_len, &r);
	handle_telemetry(a, &r, rep.count);
}

/* Breach and recovery uploads are limited to four per minute, like the server's allowance. */
static bool event_allowed(struct agent *a, int64_t now)
{
	int oldest = 0;

	for (int i = 1; i < 4; i++) {
		if (a->event_at[i] < a->event_at[oldest])
			oldest = i;
	}
	if (a->event_at[oldest] && now - a->event_at[oldest] < lp_seconds(60))
		return false;
	a->event_at[oldest] = now;
	return true;
}

static void push_sample(struct agent *a, const struct lp_sample *s)
{
	if (a->batch_count == LP_MAX_SAMPLES) {
		memmove(a->batch, a->batch + 1, (LP_MAX_SAMPLES - 1) * sizeof a->batch[0]);
		a->batch_count--;
	}
	a->batch[a->batch_count++] = *s;
}

static void open_window(struct agent *a)
{
	lp_collector_select(&a->collector, &a->cfg, lp_mono_ms(), a->force_select);
	a->force_select = false;
	lp_collect_counters(&a->collector, &a->baseline);
	a->window_open = true;
	a->window_end_ms = a->baseline.clock.mono_ms + lp_window_length_ms(a->cfg.sample_s);
}

static void close_window(struct agent *a)
{
	struct lp_counters end;
	struct lp_sample smp;
	int64_t duration, len = lp_window_length_ms(a->cfg.sample_s);
	enum lp_window_status status;
	enum lp_rule_event event = LP_RULE_NONE;

	lp_collect_counters(&a->collector, &end);
	status = lp_window_check(a->cfg.sample_s, &a->baseline.clock, &end.clock, &duration);
	if (status == LP_WINDOW_OK) {
		const struct lp_metrics *m = &smp.metrics;
		lp_collect_metrics(&a->cfg, &a->baseline, &end, duration, &smp.metrics);
		smp.end_ms = end.clock.real_ms;
		smp.duration_ms = duration;
		push_sample(a, &smp);
		event = lp_rules_feed(&a->rules, &a->cfg, &smp);
		lp_log(LP_DEBUG, "sample: cpu %.2f memory %.2f disk %.2f swap %.2f tx %.0f rx %.0f (unavailable as -1)",
		       m->ok[LP_M_CPU] ? m->value[LP_M_CPU] : -1, m->ok[LP_M_MEMORY] ? m->value[LP_M_MEMORY] : -1,
		       m->ok[LP_M_DISK] ? m->value[LP_M_DISK] : -1, m->ok[LP_M_SWAP] ? m->value[LP_M_SWAP] : -1,
		       m->ok[LP_M_NET_TX] ? m->value[LP_M_NET_TX] : -1, m->ok[LP_M_NET_RX] ? m->value[LP_M_NET_RX] : -1);
	} else {
		lp_log(status == LP_WINDOW_TOO_LONG ? LP_INFO : LP_WARN, "discarding sample: %s", lp_window_status_text(status));
	}

	if (status == LP_WINDOW_OK && end.clock.mono_ms - a->window_end_ms < 1000)
		a->window_end_ms += len;
	else
		a->window_end_ms = end.clock.mono_ms + len;
	if (lp_collector_select(&a->collector, &a->cfg, end.clock.mono_ms, false)) {
		lp_collect_counters(&a->collector, &a->baseline);
		a->window_end_ms = a->baseline.clock.mono_ms + len;
	} else {
		a->baseline = end;
	}

	if (event != LP_RULE_NONE) {
		const char *trigger = event == LP_RULE_BREACH ? "breach" : "recovery";
		lp_log(LP_INFO, event == LP_RULE_BREACH ? "threshold breach detected; uploading now" : "metrics recovered; uploading now");
		if (event_allowed(a, end.clock.mono_ms))
			upload(a, trigger);
	}
}

static void reload_key(struct agent *a)
{
	char key[LP_API_KEY_LEN + 1], err[256];

	if (lp_api_key_load(a->opt.api_key_file, key, err, sizeof err) != 0) {
		lp_log(LP_ERR, "reload failed: %s", err);
		return;
	}
	memcpy(a->key, key, sizeof key);
	lp_wipe(key, sizeof key);
	lp_transport_set_key(a->http, a->key);
	lp_log(LP_INFO, "API key reloaded");
	a->auth_failed = false;
	a->next_config_ms = lp_mono_ms();
}

static int run(struct agent *a)
{
	while (!lp_stop_requested()) {
		int64_t now = lp_mono_ms(), wake;

		if (lp_take_reload()) {
			reload_key(a);
			continue;
		}
		if (a->auth_failed) {
			lp_wait_until(LP_NEVER);
			continue;
		}
		if (now >= a->next_config_ms) {
			int rc = refresh_config(a, now);
			if (rc != 0)
				return rc;
			continue;
		}
		wake = a->next_config_ms;
		if (a->have_config && a->cfg.active) {
			if (!a->window_open) {
				open_window(a);
				continue;
			}
			if (now >= a->window_end_ms) {
				close_window(a);
				continue;
			}
			if (now >= a->next_upload_ms) {
				a->next_upload_ms += (int64_t)a->cfg.interval_s * 1000;
				if (a->next_upload_ms <= now)
					a->next_upload_ms = now + (int64_t)a->cfg.interval_s * 1000;
				upload(a, "interval");
				continue;
			}
			wake = min64(wake, min64(a->window_end_ms, a->next_upload_ms));
		}
		lp_wait_until(wake);
	}
	lp_log(LP_INFO, "stopping");
	return LP_EXIT_OK;
}

static int check(struct agent *a)
{
	struct lp_response r;
	struct lp_config c;
	char err[160];

	lp_fetch_config(a->http, NULL, &r);
	if (r.transport_error) {
		fprintf(stderr, "cannot reach %s: %s\n", a->opt.endpoint, r.error);
		return LP_EXIT_RUNTIME;
	}
	if (r.status == 401) {
		fprintf(stderr, "API key was rejected (HTTP 401)\n");
		return LP_EXIT_AUTH;
	}
	if (r.status != 200) {
		fprintf(stderr, "configuration request failed: HTTP %ld\n", r.status);
		return LP_EXIT_RUNTIME;
	}
	if (lp_config_parse(r.body, r.body_len, &c, err, sizeof err) != LP_CONFIG_OK) {
		fprintf(stderr, "invalid configuration: %s\n", err);
		return LP_EXIT_CONFIG;
	}
	printf("configuration version %lld\nupload interval %ds\nsample interval %ds\nalert after %ds above threshold\n"
	       "recover after %ds below threshold (margin %g)\nstatus %s\ndisk path %s\nnetwork interface %s\n",
	       (long long)c.config_version, c.interval_s, c.sample_s, c.breach_s, c.recovery_s, c.recovery_margin,
	       c.active ? "active" : "paused",
	       c.disk_path_valid ? c.disk_path : "(invalid)",
	       c.interface_explicit ? (c.interface_valid ? c.ifname : "(invalid)") : "(default route)");
	return LP_EXIT_OK;
}

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s [--config PATH] [--check] [--log-file PATH] [--verbose]\n\n"
		"  -c, --config PATH    configuration file (default %s)\n"
		"      --check          fetch and validate the monitor configuration, then exit\n"
		"      --log-file PATH  write logs to PATH (rotated at 1 MiB) instead of stderr\n",
		LP_AGENT_NAME, lp_default_config_path());
#ifdef _WIN32
	fprintf(out, "      --service        run under the Windows service control manager\n");
#endif
	fprintf(out,
		"  -v, --verbose        log debug messages\n"
		"  -V, --version        print the version and exit\n"
		"  -h, --help           show this help\n");
}

struct cli {
	const char *config_path;
	const char *log_file;
	bool check_only;
	bool verbose;
	bool service;
};

static int agent_main(void *arg)
{
	static struct agent a;
	const struct cli *cli = arg;
	char err[256];
	int rc;

	lp_log_init(cli->verbose);
#ifdef LP_TESTING
	const char *divisor = getenv("LP_TEST_TIME_DIVISOR");
	if (divisor && atoi(divisor) > 0)
		lp_time_divisor = atoi(divisor);
#endif
	if (lp_options_load(cli->config_path, &a.opt, err, sizeof err) != 0) {
		lp_log(LP_ERR, "%s", err);
		return LP_EXIT_CONFIG;
	}
	if (cli->log_file) {
		lp_log_to_file(cli->log_file);
	} else if (cli->service) {
		char path[LP_FILE_PATH_SIZE + 16];
		snprintf(path, sizeof path, "%s\\agent.log", a.opt.state_dir);
		lp_log_to_file(path);
	}
	if (lp_api_key_load(a.opt.api_key_file, a.key, err, sizeof err) != 0) {
		lp_log(LP_ERR, "%s", err);
		return LP_EXIT_CONFIG;
	}
	a.http = lp_transport_new(&a.opt, a.key, err, sizeof err);
	if (!a.http) {
		lp_log(LP_ERR, "%s", err);
		lp_wipe(a.key, sizeof a.key);
		return LP_EXIT_RUNTIME;
	}
	if (cli->check_only) {
		rc = check(&a);
		goto out;
	}

	rc = lp_state_open(&a.state, a.opt.state_dir, a.opt.monitor_id, a.opt.endpoint, err, sizeof err);
	if (rc != LP_STATE_OK) {
		lp_log(LP_ERR, "%s", err);
		rc = LP_EXIT_CONFIG;
		goto out;
	}
	if (a.state.new_session)
		lp_log(LP_INFO, "started a new agent session");
	if (lp_events_init() != 0) {
		lp_log(LP_ERR, "cannot install signal handlers: %s", strerror(errno));
		rc = LP_EXIT_RUNTIME;
		goto out_state;
	}
	if (lp_collector_init(&a.collector) != 0)
		lp_log(LP_WARN, "some system counters are unavailable");
	lp_log(LP_INFO, "%s %s starting for monitor %s", LP_AGENT_NAME, LP_VERSION, a.opt.monitor_id);

	lp_wait_until(lp_mono_ms() + lp_jitter_ms(lp_seconds(5)));
	a.next_config_ms = lp_mono_ms();
	rc = run(&a);
	lp_collector_cleanup();
out_state:
	lp_state_close(&a.state);
out:
	lp_transport_free(a.http);
	lp_wipe(a.key, sizeof a.key);
	return rc;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"config", required_argument, NULL, 'c'},
		{"check", no_argument, NULL, 'k'},
		{"log-file", required_argument, NULL, 'l'},
		{"service", no_argument, NULL, 's'},
		{"verbose", no_argument, NULL, 'v'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	struct cli cli = {.config_path = lp_default_config_path()};
	int opt;

	while ((opt = getopt_long(argc, argv, "c:vVh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cli.config_path = optarg;
			break;
		case 'k':
			cli.check_only = true;
			break;
		case 'l':
			cli.log_file = optarg;
			break;
		case 's':
			cli.service = true;
			break;
		case 'v':
			cli.verbose = true;
			break;
		case 'V':
			printf("%s %s\n", LP_AGENT_NAME, LP_VERSION);
			return LP_EXIT_OK;
		case 'h':
			usage(stdout);
			return LP_EXIT_OK;
		default:
			usage(stderr);
			return LP_EXIT_CONFIG;
		}
	}
	if (optind != argc || (cli.service && cli.check_only)) {
		usage(stderr);
		return LP_EXIT_CONFIG;
	}
	if (cli.service)
		return lp_service_run(agent_main, &cli);
	return agent_main(&cli);
}
