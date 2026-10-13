# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - 2026-10-13

### Added

- Linux collector for CPU, memory, swap, disk and network usage, read directly from procfs and `statvfs()`.
- Network interface selection: the dashboard setting wins; otherwise the lowest-metric IPv4 or IPv6 default route is used; an ambiguous choice is reported as unavailable.
- Protocol version 1 client. Configuration is cached with an ETag. Reports carry all six metrics, with explicit `null` values for unavailable readings.
- Session state with atomic, fsynced sequence reservations in blocks of 256, guarded by an exclusive lock.
- Response handling for each status code:
  - backoff with jitter for errors;
  - `Retry-After` honoured on 429;
  - configuration refetch on 409;
  - telemetry paused on 403;
  - sending stopped on 401 until `SIGHUP` reloads the key.
- `--check` mode for verifying an installation, with distinct exit codes for authentication (77) and configuration (78) failures.
- A hardened systemd unit and an example configuration.
- Unit tests with procfs fixtures, and an integration test against a local fake API, both run under AddressSanitizer and UndefinedBehaviorSanitizer.

[Unreleased]: https://github.com/livepulser/livepulser-agent/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/livepulser/livepulser-agent/releases/tag/v0.1.0
