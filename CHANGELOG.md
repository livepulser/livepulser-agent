# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] - 2026-11-10

First public release.

### Added

- Collectors for CPU, memory, swap, disk and network usage on Linux (procfs and `statvfs`), macOS (Mach host statistics, `sysctl` and `statfs`) and Windows (`GetSystemTimes`, `GlobalMemoryStatusEx`, paging-file performance counter, `GetDiskFreeSpaceExW` and `GetIfEntry2`).
- Network interface selection: the dashboard setting wins. Otherwise the primary IPv4 or IPv6 default route is used, and an ambiguous choice is reported as unavailable.
- Protocol version 2 client. The agent samples every `sample_seconds` (10 s by default) and uploads batches of up to 60 samples every upload interval (30–3600 s, 60 s by default). Samples that fail to upload are kept and sent with the next upload. Unavailable readings are sent as explicit `null` values.
- Configuration is cached with an ETag. Changes are picked up from the `LP-Config-Version` header on upload responses; otherwise the agent polls once an hour, or every minute while paused.
- Local threshold evaluation using the monitor's breach time, recovery time and recovery margin. A breach or recovery uploads immediately (at most four times a minute), so alerts open within the configured breach time. Changing a threshold or the recovery margin restarts the timers it affects; an open alert is kept.
- HTTPS transport using libcurl on Linux and macOS and WinHTTP on Windows. Redirects and system proxies are never followed implicitly.
- Session state with atomic, flushed sequence reservations in blocks of 256, guarded by an exclusive lock. A session the server has retired is replaced once per process after the conflict outlasts the offline lease.
- Response handling for each status code:
  - backoff with jitter for errors;
  - `Retry-After` honoured on 429;
  - configuration refetch on 409;
  - telemetry paused on 403;
  - sending stopped on 401 until the key is reloaded.
- `--check` mode for verifying an installation. It prints the sample interval and alert timing, and exits with distinct codes for authentication (77) and configuration (78) failures.
- `--log-file` with size-based rotation.
- Service integration: a hardened systemd unit, a launchd daemon with install and uninstall scripts, and a Windows service (`--service`) with PowerShell install and uninstall scripts.
- Release archives for Linux (amd64, arm64), macOS (universal) and Windows (amd64), with a `SHA256SUMS` file.
- Unit tests with procfs fixtures, and an integration test against a local fake API, run under AddressSanitizer and UndefinedBehaviorSanitizer where available. CI covers Linux (amd64, arm64), macOS (arm64, x86_64) and Windows.

[Unreleased]: https://github.com/livepulser/livepulser-agent/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/livepulser/livepulser-agent/releases/tag/v1.0.0
