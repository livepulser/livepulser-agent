#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "options.h"
#include "policy.h"
#include "procfs.h"
#include "protocol.h"
#include "state.h"

static int failures, checks;

#define CHECK(cond)                                                              \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			failures++;                                              \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-6)

static FILE *fixture(const char *name)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof path, "fixtures/proc/%s", name);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "missing fixture %s\n", path);
		exit(2);
	}
	return f;
}

static void read_stat(const char *name, struct lp_cpu *cpu, int expect)
{
	FILE *f = fixture(name);
	CHECK(lp_parse_stat(f, cpu) == expect);
	fclose(f);
}

static void test_cpu(void)
{
	struct lp_cpu a, b, c;
	double pct;

	read_stat("stat_a", &a, 0);
	read_stat("stat_b", &b, 0);
	CHECK(a.ncpu == 2);
	CHECK(a.total == 9860);
	CHECK(lp_cpu_percent(&a, &b, &pct) == 0);
	NEAR(pct, 40.0);
	CHECK(lp_cpu_percent(&a, &a, &pct) == -1);
	CHECK(lp_cpu_percent(&b, &a, &pct) == -1);

	read_stat("stat_iowait_drop", &c, 0);
	CHECK(lp_cpu_percent(&a, &c, &pct) == 0);
	NEAR(pct, 100.0 * 400 / 970);

	read_stat("stat_hotplug", &c, 0);
	CHECK(lp_cpu_percent(&a, &c, &pct) == -1);
	read_stat("stat_reset", &c, 0);
	CHECK(lp_cpu_percent(&a, &c, &pct) == -1);
	read_stat("meminfo", &c, -1);
}

static void test_memory(void)
{
	struct lp_meminfo m;
	double pct;
	FILE *f = fixture("meminfo");

	CHECK(lp_parse_meminfo(f, &m) == 0);
	fclose(f);
	CHECK(lp_memory_percent(&m, &pct) == 0);
	NEAR(pct, 25.0);
	CHECK(lp_swap_percent(&m, &pct) == 0);
	NEAR(pct, 25.0);

	f = fixture("meminfo_noavail");
	CHECK(lp_parse_meminfo(f, &m) == 0);
	fclose(f);
	CHECK(lp_memory_percent(&m, &pct) == -1);
	CHECK(lp_swap_percent(&m, &pct) == -1);
}

static void test_network(void)
{
	uint64_t rx = 0, tx = 0;
	double rate;
	char name[LP_IFNAME_SIZE] = "";
	FILE *f = fixture("net_dev");

	CHECK(lp_parse_netdev(f, "eth0", &rx, &tx) == 0);
	CHECK(rx == 1000000 && tx == 400000);
	rewind(f);
	CHECK(lp_parse_netdev(f, "docker0", &rx, &tx) == 0);
	CHECK(rx == 0 && tx == 0);
	rewind(f);
	CHECK(lp_parse_netdev(f, "eth", &rx, &tx) == -1);
	fclose(f);

	CHECK(lp_rate(1000, 61000, 60000, &rate) == 0);
	NEAR(rate, 1000.0);
	CHECK(lp_rate(5000, 1000, 60000, &rate) == -1);
	CHECK(lp_rate(0, 1000, 0, &rate) == -1);

	f = fixture("route_default");
	CHECK(lp_route_v4(f, name) == LP_ROUTE_FOUND);
	CHECK(strcmp(name, "eth0") == 0);
	fclose(f);
	f = fixture("route_ambiguous");
	CHECK(lp_route_v4(f, name) == LP_ROUTE_AMBIGUOUS);
	fclose(f);
	f = fixture("route_none");
	CHECK(lp_route_v4(f, name) == LP_ROUTE_NONE);
	fclose(f);
	f = fixture("ipv6_route");
	CHECK(lp_route_v6(f, name) == LP_ROUTE_FOUND);
	CHECK(strcmp(name, "ens3") == 0);
	fclose(f);

	CHECK(lp_valid_ifname("eth0"));
	CHECK(lp_valid_ifname("enp0s31f6.100"));
	CHECK(!lp_valid_ifname(""));
	CHECK(!lp_valid_ifname("a/b"));
	CHECK(!lp_valid_ifname(".."));
	CHECK(!lp_valid_ifname("sixteen-chars-xx"));
}

static void test_disk(void)
{
	double pct;

	CHECK(lp_disk_percent(1000, 250, &pct) == 0);
	NEAR(pct, 75.0);
	CHECK(lp_disk_percent(0, 0, &pct) == -1);
	CHECK(lp_disk_percent(10, 11, &pct) == -1);
}

static const char *valid_config =
	"{\"schema_version\":1,\"config_version\":2,\"report_interval_seconds\":60,"
	"\"settings\":{\"cpu\":80,\"memory\":80,\"disk\":80,\"swap\":80,\"network\":{\"upload\":10240,\"download\":102400},"
	"\"disk_path\":\"/var\",\"network_interface\":\"\"},"
	"\"thresholds\":{\"cpu_percent\":80},\"breach_reports\":3,\"recovery_reports\":2,"
	"\"offline_after_seconds\":210,\"active\":true,\"future_field\":{\"x\":[1,2]}}";

static enum lp_config_result parse(const char *json, struct lp_config *c)
{
	char err[160];
	return lp_config_parse(json, strlen(json), c, err, sizeof err);
}

static void test_config(void)
{
	struct lp_config c, d;
	char big[LP_MAX_CONFIG_BYTES + 2];

	CHECK(parse(valid_config, &c) == LP_CONFIG_OK);
	CHECK(c.config_version == 2 && c.interval_s == 60 && c.offline_after_s == 210 && c.active);
	CHECK(strcmp(c.disk_path, "/var") == 0 && c.disk_path_valid);
	CHECK(!c.interface_explicit);

	CHECK(parse("{\"schema_version\":1,\"config_version\":5,\"report_interval_seconds\":120,"
		    "\"settings\":{\"cpu\":90},\"thresholds\":{},\"active\":false}", &d) == LP_CONFIG_OK);
	CHECK(strcmp(d.disk_path, "/") == 0 && d.offline_after_s == 390 && !d.active);
	CHECK(lp_config_changes_window(&c, &d));
	d = c;
	d.config_version = 9;
	CHECK(!lp_config_changes_window(&c, &d));

	CHECK(parse("{\"schema_version\":1,\"config_version\":5,\"report_interval_seconds\":60,"
		    "\"settings\":{\"network_interface\":\"bad/name\",\"disk_path\":\"relative\"},\"thresholds\":{},\"active\":true}", &d) == LP_CONFIG_OK);
	CHECK(d.interface_explicit && !d.interface_valid && !d.disk_path_valid);

	CHECK(parse("{\"schema_version\":2,\"config_version\":1}", &d) == LP_CONFIG_UNSUPPORTED);
	CHECK(parse("{\"schema_version\":1,\"config_version\":0,\"report_interval_seconds\":60,\"settings\":{},\"thresholds\":{},\"active\":true}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1.5,\"report_interval_seconds\":60,\"settings\":{},\"thresholds\":{},\"active\":true}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1,\"report_interval_seconds\":3601,\"settings\":{},\"thresholds\":{},\"active\":true}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1,\"report_interval_seconds\":60,\"settings\":{},\"thresholds\":{},\"active\":1}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1,\"report_interval_seconds\":60,\"settings\":[],\"thresholds\":{},\"active\":true}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1,\"report_interval_seconds\":NaN}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("{\"schema_version\":1,\"config_version\":1e400}", &d) == LP_CONFIG_INVALID);
	CHECK(parse("not json", &d) == LP_CONFIG_INVALID);
	CHECK(parse("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]", &d) == LP_CONFIG_INVALID);
	memset(big, ' ', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHECK(parse(big, &d) == LP_CONFIG_INVALID);
}

static void test_report(void)
{
	struct lp_report r = {.boot_id = "9d1c5aef-19be-4801-bb24-ac611f4c0d6d", .sequence = 42, .config_version = 2,
			      .observed_unix_ms = 1791882000000LL, .duration_ms = 60000, .arch = "arm64"};
	char buf[2048];
	size_t len;
	cJSON *doc, *metrics, *missing;

	r.metrics.ok[LP_M_CPU] = true;
	r.metrics.value[LP_M_CPU] = 24.5;
	r.metrics.ok[LP_M_MEMORY] = true;
	r.metrics.value[LP_M_MEMORY] = 52;
	r.metrics.ok[LP_M_DISK] = true;
	r.metrics.value[LP_M_DISK] = NAN;
	r.metrics.ok[LP_M_NET_TX] = true;
	r.metrics.value[LP_M_NET_TX] = 1500.4;
	r.metrics.ok[LP_M_NET_RX] = true;
	r.metrics.value[LP_M_NET_RX] = 2e12;

	CHECK(lp_report_write(&r, buf, sizeof buf, &len) == 0);
	CHECK(len == strlen(buf) && len < LP_MAX_REPORT_BYTES);
	doc = cJSON_Parse(buf);
	CHECK(doc != NULL);
	if (doc) {
		CHECK(cJSON_GetArraySize(doc) == 9);
		CHECK(strcmp(cJSON_GetObjectItem(doc, "observed_at")->valuestring, "2026-10-13T09:00:00Z") == 0);
		CHECK(cJSON_GetObjectItem(doc, "sequence")->valuedouble == 42);
		metrics = cJSON_GetObjectItem(doc, "metrics");
		missing = cJSON_GetObjectItem(doc, "unavailable_metrics");
		CHECK(cJSON_GetArraySize(metrics) == 6);
		NEAR(cJSON_GetObjectItem(metrics, "cpu_percent")->valuedouble, 24.5);
		NEAR(cJSON_GetObjectItem(metrics, "network_tx_bytes_per_second")->valuedouble, 1500);
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(metrics, "disk_percent")));
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(metrics, "swap_percent")));
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(metrics, "network_rx_bytes_per_second")));
		CHECK(cJSON_GetArraySize(missing) == 3);
		CHECK(strcmp(cJSON_GetArrayItem(missing, 0)->valuestring, "disk_percent") == 0);
		CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "agent"), "name")->valuestring, "livepulser-agent") == 0);
		cJSON_Delete(doc);
	}

	memset(&r.metrics, 0, sizeof r.metrics);
	CHECK(lp_report_write(&r, buf, sizeof buf, &len) == 0);
	CHECK(strstr(buf, "\"unavailable_metrics\":[\"cpu_percent\",\"memory_percent\",\"disk_percent\",\"swap_percent\","
			  "\"network_tx_bytes_per_second\",\"network_rx_bytes_per_second\"]") != NULL);

	CHECK(lp_report_write(&r, buf, 64, &len) == -1);
	r.duration_ms = 3600001;
	CHECK(lp_report_write(&r, buf, sizeof buf, &len) == -1);
	r.duration_ms = 60000;
	r.sequence = LP_MAX_SEQUENCE + 1;
	CHECK(lp_report_write(&r, buf, sizeof buf, &len) == -1);
}

static void test_options(void)
{
	struct lp_options o;
	char err[256];
	const char *ok = "# comment\n endpoint = https://rs.livepulser.com/ \nmonitor_id=6F1C2E9A-4B7D-4C3E-8A5F-0D2B9E7C1A34\n"
			 "api_key_file=/etc/livepulser-agent/api-key\nstate_dir=/var/lib/livepulser-agent\n";

	CHECK(lp_options_parse(ok, strlen(ok), &o, err, sizeof err) == 0);
	CHECK(strcmp(o.endpoint, "https://rs.livepulser.com") == 0);
	CHECK(strcmp(o.monitor_id, "6f1c2e9a-4b7d-4c3e-8a5f-0d2b9e7c1a34") == 0);
	CHECK(o.proxy[0] == '\0');

#define BAD(text) CHECK(lp_options_parse(text, strlen(text), &o, err, sizeof err) == -1)
	BAD("endpoint=http://rs.livepulser.com\n");
	BAD("endpoint=https://rs.livepulser.com/v1\n");
	BAD("endpoint=https://user@rs.livepulser.com\n");
	BAD("endpoint=ftp://rs.livepulser.com\n");
	BAD("monitor_id=not-a-uuid\n");
	BAD("api_key_file=relative/path\n");
	BAD("unknown=1\n");
	BAD("endpoint\n");
	BAD("endpoint=https://a\nendpoint=https://b\n");
#undef BAD
	CHECK(lp_options_parse("endpoint=http://127.0.0.1:8080\n", 31, &o, err, sizeof err) == 0);
}

static void write_file(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		exit(2);
	fchmod(fd, mode);
	close(fd);
}

static void test_api_key(const char *dir)
{
	char path[512], key[LP_API_KEY_LEN + 1], err[256];

	snprintf(path, sizeof path, "%s/key", dir);
	write_file(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123-_6\n", 0600);
	CHECK(lp_api_key_load(path, key, err, sizeof err) == 0);
	CHECK(strcmp(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123-_6") == 0);
	write_file(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123-_6\n", 0644);
	CHECK(lp_api_key_load(path, key, err, sizeof err) == -1);
	write_file(path, "short\n", 0600);
	CHECK(lp_api_key_load(path, key, err, sizeof err) == -1);
	write_file(path, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123+/6\n", 0600);
	CHECK(lp_api_key_load(path, key, err, sizeof err) == -1);
}

static void test_state(const char *dir)
{
	struct lp_state s, other;
	char err[256], boot[LP_UUID_LEN + 1], path[512];
	int64_t seq = 0;
	const char *monitor = "6f1c2e9a-4b7d-4c3e-8a5f-0d2b9e7c1a34";

	CHECK(lp_state_open(&s, dir, monitor, "https://rs.livepulser.com", err, sizeof err) == LP_STATE_OK);
	CHECK(s.new_session && !s.recovered_corrupt);
	CHECK(lp_valid_uuid(s.boot_id));
	memcpy(boot, s.boot_id, sizeof boot);
	CHECK(lp_state_open(&other, dir, monitor, "https://rs.livepulser.com", err, sizeof err) == LP_STATE_LOCKED);
	CHECK(lp_state_take_sequence(&s, &seq, err, sizeof err) == 0 && seq == 1);
	CHECK(s.reserved_through == LP_STATE_BLOCK);
	for (int i = 0; i < LP_STATE_BLOCK; i++)
		CHECK(lp_state_take_sequence(&s, &seq, err, sizeof err) == 0);
	CHECK(seq == LP_STATE_BLOCK + 1 && s.reserved_through == 2 * LP_STATE_BLOCK);
	lp_state_close(&s);

	CHECK(lp_state_open(&s, dir, monitor, "https://rs.livepulser.com", err, sizeof err) == LP_STATE_OK);
	CHECK(!s.new_session && strcmp(s.boot_id, boot) == 0);
	CHECK(lp_state_take_sequence(&s, &seq, err, sizeof err) == 0 && seq == 2 * LP_STATE_BLOCK + 1);
	lp_state_close(&s);

	CHECK(lp_state_open(&s, dir, "11111111-2222-4333-8444-555555555555", "https://rs.livepulser.com", err, sizeof err) == LP_STATE_OK);
	CHECK(s.new_session && !s.recovered_corrupt && strcmp(s.boot_id, boot) != 0);
	lp_state_close(&s);

	snprintf(path, sizeof path, "%s/state", dir);
	write_file(path, "format_version=1\nboot_id=garbage\n", 0600);
	CHECK(lp_state_open(&s, dir, monitor, "https://rs.livepulser.com", err, sizeof err) == LP_STATE_OK);
	CHECK(s.new_session && s.recovered_corrupt && s.next_sequence == 1);
	lp_state_close(&s);
	snprintf(path, sizeof path, "%s/state.corrupt", dir);
	CHECK(access(path, F_OK) == 0);
}

static void test_policy(void)
{
	struct lp_guard g = {{0, 0}};
	struct lp_clock a = {1000, 1000, 1700000000000LL}, b;
	int64_t d;

	CHECK(lp_guard_next(&g, 100000) == 100000);
	lp_guard_record(&g, 100000);
	CHECK(lp_guard_next(&g, 101000) == 101000);
	lp_guard_record(&g, 101000);
	CHECK(lp_guard_next(&g, 102000) == 160000);
	CHECK(lp_guard_next(&g, 160000) == 160000);

	CHECK(lp_backoff_ms(1) >= 60000 && lp_backoff_ms(1) <= 65000);
	CHECK(lp_backoff_ms(2) >= 120000 && lp_backoff_ms(2) <= 125000);
	CHECK(lp_backoff_ms(3) >= 240000 && lp_backoff_ms(3) <= 245000);
	CHECK(lp_backoff_ms(9) >= 300000 && lp_backoff_ms(9) <= 305000);

	CHECK(lp_window_length_ms(60) == 60000);
	CHECK(lp_window_length_ms(3600) == 3599000);

	b = (struct lp_clock){61000, 61000, a.real_ms + 60000};
	CHECK(lp_window_check(60, &a, &b, &d) == LP_WINDOW_OK && d == 60000);
	b = (struct lp_clock){20000, 20000, a.real_ms + 19000};
	CHECK(lp_window_check(60, &a, &b, &d) == LP_WINDOW_TOO_SHORT);
	b = (struct lp_clock){100000, 100000, a.real_ms + 99000};
	CHECK(lp_window_check(60, &a, &b, &d) == LP_WINDOW_TOO_LONG);
	b = (struct lp_clock){61000, 361000, a.real_ms + 360000};
	CHECK(lp_window_check(60, &a, &b, &d) == LP_WINDOW_SUSPENDED);
	b = (struct lp_clock){61000, 61000, a.real_ms + 3660000};
	CHECK(lp_window_check(60, &a, &b, &d) == LP_WINDOW_CLOCK_JUMP);
	b = (struct lp_clock){1000 + 3600001, -1, a.real_ms + 3600001};
	a.boot_ms = -1;
	CHECK(lp_window_check(3600, &a, &b, &d) == LP_WINDOW_TOO_LONG);

	CHECK(!lp_retry_allowed(60, 60000, 61000));
	CHECK(lp_retry_allowed(600, 60000, 61000));
	CHECK(!lp_retry_allowed(600, 120000, 121000));
	CHECK(lp_retry_allowed(3600, 120000, 121000));
	CHECK(!lp_retry_allowed(3600, 240000, 241000));
}

int main(void)
{
	char dir[] = "/tmp/lp-unit-XXXXXX";

	if (!mkdtemp(dir))
		return 2;
	chmod(dir, 0700);
	test_cpu();
	test_memory();
	test_network();
	test_disk();
	test_config();
	test_report();
	test_options();
	test_api_key(dir);
	test_state(dir);
	test_policy();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
