# Agent protocol (version 1)

This document describes what the official agent sends and how it reacts to responses. Third-party agents can implement the same contract. The server is authoritative; if the two ever disagree, the server's behaviour wins.

All requests go to the configured origin (`https://rs.livepulser.com`). They carry `Authorization: Bearer <api key>`, where the key is the 43-character key issued for the monitor. The key is never placed in a URL.

## Configuration

```
GET /v1/resource/{monitor_id}/config
```

The response is the configuration object itself, with no wrapper:

```json
{
  "schema_version": 1,
  "config_version": 2,
  "report_interval_seconds": 60,
  "settings": {"cpu": 80, "disk_path": "/", "network_interface": ""},
  "thresholds": {"cpu_percent": 80},
  "breach_reports": 3,
  "recovery_reports": 2,
  "offline_after_seconds": 210,
  "active": true
}
```

Validation:

- An unknown `schema_version` makes the agent exit with code 78.
- `config_version` must be a positive integer.
- `report_interval_seconds` must be an integer from 60 to 3600.
- `active` must be a boolean.
- `settings` and `thresholds` must be objects. Within `settings`, `disk_path` and `network_interface` are optional strings.
- Unknown fields are ignored.
- The body is limited to 16 KiB and nesting to 16 levels.

Refreshing:

- The agent refreshes every five minutes plus 0–30 s of jitter. Each refresh sends `If-None-Match` with the last `ETag`, and a `304` keeps the cached object.
- A malformed response never replaces a valid configuration.
- When `active` is false, the agent stops measuring and keeps refreshing on the same cadence.

## Telemetry

```
POST /v1/resource/{monitor_id}/telemetry
Content-Type: application/json
```

```json
{
  "schema_version": 1,
  "boot_id": "9d1c5aef-19be-4801-bb24-ac611f4c0d6d",
  "sequence": 42,
  "config_version": 2,
  "observed_at": "2026-10-13T09:00:00Z",
  "sample_duration_ms": 60000,
  "agent": {"name": "livepulser-agent", "version": "0.1.0", "os": "linux", "arch": "amd64"},
  "metrics": {
    "cpu_percent": 24.5,
    "memory_percent": 52.0,
    "disk_percent": 41.5,
    "swap_percent": null,
    "network_tx_bytes_per_second": 1500,
    "network_rx_bytes_per_second": 3500
  },
  "unavailable_metrics": ["swap_percent"]
}
```

- **All six metrics are always present.** Unavailable readings are `null` and listed exactly once in `unavailable_metrics`.
- **Value ranges.** Percentages are 0–100. Rates are 0–10¹² bytes per second.
- **No extra fields.** Unknown fields are rejected, and the body must be under 8 KiB.
- **Duration.** `sample_duration_ms` must be within 0.5–1.5× the interval and at most 3,600,000.
- **Observation time.** `observed_at` must be no more than five minutes old and no more than two minutes in the future.
- **Agent identity.** `agent.os` is `linux`, `darwin` or `windows`, and `agent.arch` is `amd64` or `arm64`. The dashboard uses them to show the server platform.
- **Session.** `boot_id` identifies an agent session. It is a random UUID kept in the state directory, not the kernel boot ID.
- **Sequence numbers.** Within a session, `sequence` increases strictly and never exceeds 2⁵³−1. The agent reserves blocks of 256 numbers with an atomic, fsynced state write. After a restart it skips the rest of the reserved block, so a number is never reused. Gaps are expected.

## Responses

| Status | Agent behaviour |
| --- | --- |
| 202 | Accepted (or an exact duplicate). Continue at the normal cadence. |
| 401 | Stop sending and wait for a key reload (`SIGHUP`, or `paramchange` on Windows). At startup, exit with code 77. |
| 403 | The monitor is paused or not provisioned. Refresh configuration and back off. |
| 409 | Configuration version or session conflict. Refetch configuration once. If the version is unchanged, back off for at least the offline lease. The boot ID is never regenerated. |
| 413 / 422 | Drop the report and log the reason, including clock skew taken from the `Date` header. Back off after repeated rejections. |
| 429 | Honour `Retry-After` (at least 60 s) plus jitter. |
| 5xx, network, TLS | Back off 60, 120, 240, then 300 s, plus 0–5 s jitter. A server `Retry-After` wins if it is longer. |
| 3xx | Never followed. Logged as an endpoint misconfiguration. |

## Rate behaviour

- **Normal traffic.** About 1,440 telemetry requests and 288 configuration requests per day at the default interval.
- **Client-side caps.** At most two telemetry and two configuration requests per rolling minute.
- **Retries.** A report is retried at most once, with an identical body. The retry happens only when it can land within 0.2 of the interval, so the next report stays at least 0.8 of the interval away, and within four minutes of `observed_at`. Otherwise the next fresh window is sent instead.
- **No backlog.** Windows that end during a back-off are discarded. Nothing is queued or replayed after an outage.

## Session ownership

Only one session can own a monitor. When the local state is lost, the agent starts a new session. The server accepts a new session only after the old one has been silent for `offline_after_seconds`, which is 3×interval + 30 s. If the state file is corrupt, the agent waits that long before its first report.
