# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-10-13

### Added

- Collectors for CPU, memory, swap, disk and network usage on Linux (procfs and `statvfs`), macOS (Mach host statistics, `sysctl` and `statfs`) and Windows (`GetSystemTimes`, `GlobalMemoryStatusEx`, paging-file performance counter, `GetDiskFreeSpaceExW` and `GetIfEntry2`).
- Network interface selection: the dashboard setting wins. Otherwise the primary IPv4 or IPv6 default route is used, and an ambiguous choice is reported as unavailable.
- Protocol version 1 client. Configuration is cached with an ETag, and each report carries all six metrics, with explicit `null` values for unavailable readings.
- HTTPS transport using libcurl on Linux and macOS and WinHTTP on Windows. Redirects and system proxies are never followed implicitly.
- Session state with atomic, flushed sequence reservations in blocks of 256, guarded by an exclusive lock.
- Response handling for each status code:
  - backoff with jitter for errors;
  - `Retry-After` honoured on 429;
  - configuration refetch on 409;
  - telemetry paused on 403;
  - sending stopped on 401 until the key is reloaded.
- `--check` mode for verifying an installation, with distinct exit codes for authentication (77) and configuration (78) failures.
- `--log-file` with size-based rotation.
- Service integration: a hardened systemd unit, a launchd daemon with install and uninstall scripts, and a Windows service (`--service`) with PowerShell install and uninstall scripts.
- Unit tests with procfs fixtures, and an integration test against a local fake API, run under AddressSanitizer and UndefinedBehaviorSanitizer where available. CI covers Linux (amd64, arm64), macOS (arm64, x86_64) and Windows.

[Unreleased]: https://github.com/livepulser/livepulser-agent/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/livepulser/livepulser-agent/releases/tag/v0.1.0
