# Agent protocol (version 2)

This document describes what the official agent sends and how it reacts to responses, so third-party agents can implement the same contract. The server is authoritative; if this document and the server ever disagree, the server wins.

All requests go to the configured origin (normally `https://rs.livepulser.com`) with `Authorization: Bearer <api key>`, where the key is the 43-character key issued for the monitor. The key never appears in a URL.

## Configuration

```
GET /v1/resource/{monitor_id}/config
```

```json
{
  "schema_version": 2,
  "config_version": 12,
  "report_interval_seconds": 60,
  "sample_seconds": 10,
  "settings": {"cpu": 80, "disk_path": "/", "network_interface": ""},
  "thresholds": {"cpu_percent": 80},
  "rules": {"breach_seconds": 30, "recovery_seconds": 60, "recovery_margin": 0},
  "offline_after_seconds": 210,
  "active": true
}
```

- `report_interval_seconds` (30–3600) is how often the agent uploads; `sample_seconds` is how often it measures.
- `thresholds` uses the metric names below. `rules` decides when an alert opens and recovers (see [Alerts](#alerts)).
- Unknown fields are ignored; an unknown `schema_version` makes the agent exit with code 78. The body is limited to 16 KiB and nesting to 16 levels.
- The response carries an `ETag`; refreshes send `If-None-Match` and a 304 keeps the cached configuration.

**When the agent fetches configuration:** at start-up, whenever an upload response's `LP-Config-Version` header differs from the version it runs, every 60 s while the monitor is paused, and once an hour as a safety net. Dashboard changes therefore reach a running agent with its next upload, without polling.

## Telemetry

```
POST /v1/resource/{monitor_id}/telemetry
Content-Type: application/json
```

```json
{
  "schema_version": 2,
  "boot_id": "9d1c5aef-19be-4801-bb24-ac611f4c0d6d",
  "sequence": 42,
  "config_version": 12,
  "trigger": "interval",
  "agent": {"name": "livepulser-agent", "version": "0.2.0", "os": "linux", "arch": "amd64"},
  "start_ms": 1791882000000,
  "samples": [
    [10000, 10000, 24.5, 52.0, 41.5, null, 1500, 3500],
    [20000, 10000, 26.1, 52.0, 41.5, null, 1720, 3100]
  ]
}
```

- Each sample is `[end_offset_ms, duration_ms, cpu_percent, memory_percent, disk_percent, swap_percent, network_tx_bytes_per_second, network_rx_bytes_per_second]`, with offsets relative to `start_ms` (Unix milliseconds). Samples are contiguous and ordered.
- One upload carries 1–60 samples and must stay under 8 KiB. Unavailable readings are `null`; percentages are 0–100 and rates 0–10¹² bytes per second.
- The last sample must end within five minutes before or two minutes after server time.
- `trigger` is `interval` for the regular upload, or `breach` / `recovery` when the agent's local evaluation changes state. Those upload immediately, at most four per minute.
- `boot_id` identifies an agent session: a random UUID kept in the state directory. `sequence` increases strictly within a session and is reserved on disk in blocks of 256, so it is never reused after a restart.
- `agent.os` is `linux`, `darwin` or `windows`; the dashboard shows it as the server platform.

Samples that fail to upload stay buffered (up to 60) and go out with the next upload; the server ignores samples it has already counted, so retries never double count.

## Responses

| Status | Agent behaviour |
| --- | --- |
| 202 | Accepted (or an exact duplicate). Buffered samples are released. |
| 401 | Stop sending until the key is reloaded (`SIGHUP`, or `paramchange` on Windows). At start-up, exit with code 77. |
| 403 | The monitor is paused or not provisioned. Drop buffered samples and refresh configuration. |
| 409 | `LP-Conflict: sequence` or `session`. Back off; if the conflict outlasts the offline lease plus one interval, start one new session (at most once per process). |
| 413 / 422 | Drop the samples and log the reason from the response body, including clock skew from the `Date` header. |
| 429 | Honour `Retry-After` (at least 60 s) plus jitter. |
| 5xx, network, TLS | Back off 60, 120, 240, then 300 s plus jitter; keep the samples. |
| 3xx | Never followed; logged as an endpoint misconfiguration. |

Every response to an authenticated upload carries `LP-Config-Version`.

## Alerts

The server evaluates alerts from the samples; the agent runs the same rule locally only to decide when to upload immediately.

- A metric **breaches** once it has stayed at or above its threshold for `breach_seconds`.
- An alert **recovers** once every metric that caused it has stayed at or below its recovery level for `recovery_seconds`. The recovery level is `threshold − recovery_margin` for percentages and `threshold × (1 − recovery_margin / 100)` for network rates. Metrics that did not cause the alert never block recovery.
- A configuration change keeps an open alert, but restarts the timers of any metric whose threshold changed, and every recovery timer when `recovery_margin` changes.
- A gap in samples (for example after the host slept) restarts the timers.
- If no upload arrives for `offline_after_seconds` (3 × upload interval + 30 s), the server marks the agent offline; the next upload closes an offline-only incident.

## Session ownership

Only one session owns a monitor. The server accepts a new session only after the old one has been silent for `offline_after_seconds`, and retires the old boot ID for 24 hours. When local state is lost the agent starts a new session; if the state file was corrupt it first waits for that lease.
