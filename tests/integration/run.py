#!/usr/bin/env python3
"""End-to-end test: runs a test build of the agent against a local fake resource API."""

import hashlib
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

AGENT = os.path.abspath(sys.argv[1])
MONITOR = "6f1c2e9a-4b7d-4c3e-8a5f-0d2b9e7c1a34"
KEY = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123-_6"
OTHER_KEY = "Zbcdefghijklmnopqrstuvwxyz0123456789ABCDE_-"
INTERVAL = 2
LEASE = 3
SYSTEM = platform.system()
FIXTURE = SYSTEM == "Linux"
IFACE = {"Linux": "lo", "Darwin": "lo0", "Windows": "Loopback Pseudo-Interface 1"}[SYSTEM]
METRICS = ["cpu_percent", "memory_percent", "disk_percent", "swap_percent",
           "network_tx_bytes_per_second", "network_rx_bytes_per_second"]


class Api:
    def __init__(self):
        self.lock = threading.Lock()
        self.version = 1
        self.active = True
        self.schema = 1
        self.key = KEY
        self.forced = []
        self.log = []
        self.boot = None
        self.seq = 0
        self.last = 0.0
        self.retired = set()

    def config(self):
        body = json.dumps({
            "schema_version": self.schema, "config_version": self.version,
            "report_interval_seconds": INTERVAL,
            "settings": {"cpu": 80, "disk_path": "/", "network_interface": IFACE},
            "thresholds": {"cpu_percent": 80}, "breach_reports": 3, "recovery_reports": 2,
            "offline_after_seconds": INTERVAL * 3 + 30, "active": self.active,
        }).encode()
        return body, '"%s"' % hashlib.sha256(body).hexdigest()

    def requests(self, kind, status=None):
        with self.lock:
            return [r for r in self.log if r["kind"] == kind and (status is None or r["status"] == status)]


API = Api()


def validate(report):
    expected = {"schema_version", "boot_id", "sequence", "config_version", "observed_at",
                "sample_duration_ms", "agent", "metrics", "unavailable_metrics"}
    assert set(report) == expected, report.keys()
    assert set(report["metrics"]) == set(METRICS)
    nulls = [k for k in METRICS if report["metrics"][k] is None]
    assert sorted(nulls) == sorted(report["unavailable_metrics"]), report
    assert 1000 <= report["sample_duration_ms"] <= 3600000
    assert INTERVAL * 500 <= report["sample_duration_ms"] <= INTERVAL * 1500
    observed = datetime.strptime(report["observed_at"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    assert abs(observed.timestamp() - time.time()) < 10
    assert report["agent"]["name"] == "livepulser-agent"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def record(self, kind, status, body=None):
        API.log.append({"kind": kind, "status": status, "time": time.monotonic(),
                        "headers": dict(self.headers), "body": body})

    def authorized(self):
        return self.headers.get("Authorization") == "Bearer " + API.key

    def do_GET(self):
        with API.lock:
            if self.path != "/v1/resource/%s/config" % MONITOR:
                return self.reply(404)
            if not self.authorized():
                self.record("config", 401)
                return self.reply(401)
            body, etag = API.config()
            if self.headers.get("If-None-Match") == etag:
                self.record("config", 304)
                return self.reply(304, headers={"ETag": etag})
            self.record("config", 200)
            self.reply(200, body, {"ETag": etag, "Content-Type": "application/json"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        with API.lock:
            if self.path != "/v1/resource/%s/telemetry" % MONITOR:
                return self.reply(404)
            report = json.loads(raw)
            validate(report)
            if API.forced:
                status, headers = API.forced.pop(0)
                self.record("telemetry", status, report)
                return self.reply(status, headers=headers)
            status = 202
            if not self.authorized():
                status = 401
            elif not API.active:
                status = 403
            elif report["config_version"] != API.version:
                status = 409
            elif API.boot == report["boot_id"] and report["sequence"] == API.seq:
                status = 202
            elif API.boot == report["boot_id"] and report["sequence"] < API.seq:
                status = 409
            elif API.boot not in (None, report["boot_id"]) and (
                    report["boot_id"] in API.retired or time.monotonic() - API.last < LEASE):
                status = 409
            elif time.monotonic() - API.last < INTERVAL * 0.8:
                status = 429
            if status == 202:
                if API.boot not in (None, report["boot_id"]):
                    API.retired.add(API.boot)
                API.boot, API.seq, API.last = report["boot_id"], report["sequence"], time.monotonic()
            self.record("telemetry", status, report)
            self.reply(status, headers={"Retry-After": "60"} if status == 429 else None)


def write_atomic(path, text):
    with open(path + ".tmp", "w") as f:
        f.write(text)
    os.replace(path + ".tmp", path)


class FakeProc(threading.Thread):
    """Counters follow elapsed time: 30% CPU, 1000 B/s upload, 2000 B/s download.

    Values are derived from the clock rather than counted per tick, so a slow test host
    only makes the files staler, never the long-run rates wrong.
    """

    def __init__(self, root):
        super().__init__(daemon=True)
        self.root = root
        self.stopped = threading.Event()
        self.start_time = time.monotonic()
        os.makedirs(os.path.join(root, "net"))
        write_atomic(os.path.join(root, "meminfo"),
                     "MemTotal: 4000000 kB\nMemAvailable: 3000000 kB\nSwapTotal: 2000000 kB\nSwapFree: 1500000 kB\n")
        write_atomic(os.path.join(root, "net", "route"), "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\n")
        self.write()

    def write(self):
        elapsed = time.monotonic() - self.start_time
        ticks = int(elapsed * 100)
        user, idle = 1000 + 30 * ticks, 5000 + 70 * ticks
        write_atomic(os.path.join(self.root, "stat"),
                     "cpu  %d 0 0 %d 0 0 0 0 0 0\ncpu0 %d 0 0 %d 0 0 0 0 0 0\nintr 1\n" % (user, idle, user, idle))
        write_atomic(os.path.join(self.root, "net", "dev"),
                     "Inter-| Receive | Transmit\n face |bytes packets|bytes packets\n"
                     "%s: %d 0 0 0 0 0 0 0 %d 0 0 0 0 0 0 0\n" % (IFACE.rjust(6), int(2000 * elapsed), int(1000 * elapsed)))

    def run(self):
        while not self.stopped.wait(0.1):
            self.write()


def wait_for(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + what)


class Agent:
    def __init__(self, work, conf, *args):
        self.log_path = os.path.join(work, "agent-%d.log" % time.monotonic_ns())
        env = dict(os.environ, LP_TEST_TIME_DIVISOR="60", ASAN_OPTIONS="detect_leaks=%d" % FIXTURE,
                   https_proxy="http://127.0.0.1:9", http_proxy="http://127.0.0.1:9")
        if FIXTURE:
            env["LP_TEST_PROC_ROOT"] = os.path.join(work, "proc")
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if SYSTEM == "Windows" else 0
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([AGENT, "--config", conf, "--verbose"] + list(args),
                                     stdout=subprocess.PIPE, stderr=self.log, env=env, creationflags=flags)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.CTRL_BREAK_EVENT if SYSTEM == "Windows" else signal.SIGTERM)
        code = self.proc.wait(timeout=15)
        self.log.close()
        return code

    def output(self):
        with open(self.log_path) as f:
            return f.read()


def rotate_key_posix(agent, key_file):
    with API.lock:
        API.key = OTHER_KEY
    wait_for(lambda: API.requests("telemetry", 401), 10, "a 401")
    time.sleep(0.5)
    total = len(API.log)
    time.sleep(4)
    assert len(API.log) == total, "requests continued after 401"
    write_atomic(key_file, OTHER_KEY + "\n")
    os.chmod(key_file, 0o600)
    count = len(accepted())
    agent.proc.send_signal(signal.SIGHUP)
    wait_for(lambda: len(accepted()) > count, 15, "reports after key reload")
    print("ok   401 stops requests until SIGHUP reloads the key")
    return OTHER_KEY


def rotate_key_windows(agent, key_file):
    """Console runs have no reload signal; the service uses SERVICE_CONTROL_PARAMCHANGE instead."""
    with API.lock:
        API.key = OTHER_KEY
    wait_for(lambda: API.requests("telemetry", 401), 10, "a 401")
    time.sleep(0.5)
    total = len(API.log)
    time.sleep(4)
    assert len(API.log) == total, "requests continued after 401"
    print("ok   401 stops requests")
    with API.lock:
        API.key = KEY
    return KEY


def accepted():
    return API.requests("telemetry", 202)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    work = tempfile.mkdtemp(prefix="lp-integration-")
    os.chmod(work, 0o700)
    state = os.path.join(work, "state")
    os.mkdir(state, 0o700)
    key_file = os.path.join(work, "api-key")
    write_atomic(key_file, KEY + "\n")
    os.chmod(key_file, 0o600)
    conf = os.path.join(work, "agent.conf")
    write_atomic(conf, "endpoint=http://127.0.0.1:%d\nmonitor_id=%s\napi_key_file=%s\nstate_dir=%s\n"
                 % (server.server_address[1], MONITOR.upper(), key_file, state))
    proc = FakeProc(os.path.join(work, "proc"))
    if FIXTURE:
        proc.start()

    check = subprocess.run([AGENT, "--config", conf, "--check"], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    assert check.returncode == 0 and "configuration version 1" in check.stdout, check
    print("ok   --check prints the remote configuration")

    agent = Agent(work, conf)
    wait_for(lambda: len(accepted()) >= 3, 20, "three accepted reports")
    first = accepted()[:3]
    assert API.requests("config")[0]["time"] < first[0]["time"]
    assert [r["body"]["sequence"] for r in first] == [1, 2, 3]
    for r in first:
        m = r["body"]["metrics"]
        assert r["body"]["agent"]["os"] == {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}[SYSTEM]
        assert m["disk_percent"] is not None
        if FIXTURE:
            assert abs(m["cpu_percent"] - 30.0) < 0.01, m
            assert abs(m["memory_percent"] - 25.0) < 0.01 and abs(m["swap_percent"] - 25.0) < 0.01, m
            assert 600 <= m["network_tx_bytes_per_second"] <= 1400, m
            assert 1200 <= m["network_rx_bytes_per_second"] <= 2800, m
        else:
            for k in ("cpu_percent", "memory_percent", "disk_percent"):
                assert m[k] is not None and 0 <= m[k] <= 100, m
            assert m["network_tx_bytes_per_second"] is not None and m["network_rx_bytes_per_second"] is not None, m
        assert r["headers"]["Content-Type"] == "application/json"
    gaps = [b["time"] - a["time"] for a, b in zip(first, first[1:])]
    assert all(INTERVAL * 0.8 <= g <= INTERVAL * 1.3 for g in gaps), gaps
    print("ok   healthy reports carry %s metrics at the configured cadence" % ("exact fixture" if FIXTURE else "native"))

    wait_for(lambda: API.requests("config", 304), 15, "conditional config refresh")
    print("ok   config refresh uses If-None-Match and accepts 304")

    with API.lock:
        API.version = 2
    wait_for(lambda: accepted()[-1]["body"]["config_version"] == 2, 15, "reports with config version 2")
    assert API.requests("telemetry", 409), "expected a 409 before the refetch"
    print("ok   409 triggers a config refetch and new reports use the new version")

    with API.lock:
        API.forced = [(429, {"Retry-After": "60"})]
    wait_for(lambda: API.requests("telemetry", 429), 10, "a 429 response")
    limited = API.requests("telemetry", 429)[-1]["time"]
    wait_for(lambda: accepted()[-1]["time"] > limited, 15, "a report after the rate limit")
    assert accepted()[-1]["time"] - limited >= 0.9
    print("ok   429 Retry-After is honored")

    with API.lock:
        API.forced = [(503, None)]
    wait_for(lambda: API.requests("telemetry", 503), 10, "a 503 response")
    failed = API.requests("telemetry", 503)[-1]["time"]
    wait_for(lambda: accepted()[-1]["time"] > failed, 15, "a report after the outage")
    assert accepted()[-1]["time"] - failed >= 0.9
    print("ok   503 backs off before the next report")

    with API.lock:
        API.active = False
        paused_at = time.monotonic()
    wait_for(lambda: any(r["time"] > paused_at for r in API.requests("config", 200)), 15,
             "the paused configuration to be fetched")
    time.sleep(1)
    paused_count = len(API.requests("telemetry"))
    time.sleep(3)
    assert len(API.requests("telemetry")) == paused_count, "telemetry continued while paused"
    with API.lock:
        API.active = True
    count = len(accepted())
    wait_for(lambda: len(accepted()) > count, 20, "reports after resume")
    print("ok   pausing suspends telemetry and resuming restarts it")

    file_key = (rotate_key_windows if SYSTEM == "Windows" else rotate_key_posix)(agent, key_file)

    second = Agent(work, conf)
    assert second.proc.wait(timeout=10) == 78 and "another agent instance" in second.output()
    print("ok   the state lock rejects a second instance")

    boot = accepted()[-1]["body"]["boot_id"]
    last_seq = accepted()[-1]["body"]["sequence"]
    assert agent.stop() == 0
    log = agent.output()
    assert KEY not in log and OTHER_KEY not in log
    print("ok   SIGTERM exits cleanly and logs never contain the key")

    count = len(accepted())
    agent = Agent(work, conf)
    wait_for(lambda: len(accepted()) > count, 20, "reports after restart")
    resumed = accepted()[count]["body"]
    assert resumed["boot_id"] == boot and resumed["sequence"] > max(last_seq, 256), resumed
    assert agent.stop() == 0
    print("ok   restart keeps the session and abandons reserved sequence numbers")

    other_state = os.path.join(work, "state-other")
    os.mkdir(other_state, 0o700)
    other_conf = os.path.join(work, "agent-other.conf")
    with open(conf) as f:
        write_atomic(other_conf, f.read().replace(state, other_state))
    count = len(accepted())
    other = Agent(work, other_conf)
    wait_for(lambda: any(r["body"]["boot_id"] != boot for r in accepted()[count:]), 40, "another host to take over")
    assert other.stop() == 0
    count = len(accepted())
    agent = Agent(work, conf)
    wait_for(lambda: len(accepted()) > count, 40, "the original host to start a new session")
    renewed = accepted()[-1]["body"]
    assert agent.stop() == 0
    assert renewed["boot_id"] not in API.retired and renewed["boot_id"] != boot and renewed["sequence"] == 1, renewed
    assert "starting a new session" in agent.output()
    print("ok   a session retired by another host is replaced once")

    with API.lock:
        API.schema = 2
    agent = Agent(work, conf)
    assert agent.proc.wait(timeout=15) == 78
    assert "unsupported configuration schema_version 2" in agent.output()
    agent.stop()
    with API.lock:
        API.schema = 1
        API.key = KEY if file_key == OTHER_KEY else OTHER_KEY
    check = subprocess.run([AGENT, "--config", conf, "--check"], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    assert check.returncode == 77, check
    print("ok   unsupported schema exits 78 and a rejected key exits 77")

    if FIXTURE:
        proc.stopped.set()
    server.shutdown()
    print("integration tests passed")


if __name__ == "__main__":
    main()
