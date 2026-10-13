# Contributing

Issues and pull requests are welcome.

- Keep the agent small. New dependencies, threads, background helpers or per-report disk writes need a strong reason.
- Never invent measurements. If a value cannot be measured reliably, report it as unavailable.
- Protocol changes must match the LivePulser server. Open an issue before changing the report format.
- Run `make test` before sending a change. It builds with `-Werror` and runs the unit and integration tests under ASan/UBSan. CI repeats this on amd64 and arm64.
- Follow the existing style: C17, tabs, bounded buffers, checked conversions, and comments only where the reason is not obvious from the code.
- Add an entry under `Unreleased` in `CHANGELOG.md` for any user-visible change.
