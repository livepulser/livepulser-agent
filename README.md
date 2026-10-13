# livepulser-agent

A small, auditable C agent that reports server resource usage to [LivePulser](https://livepulser.com) resource monitors.

Each configured interval (60 seconds by default) the agent measures six values and sends them in one HTTPS request:

| Metric | Source |
| --- | --- |
| CPU usage % | `/proc/stat`, all CPUs over the interval |
| Memory usage % | `/proc/meminfo`, `(MemTotal − MemAvailable) / MemTotal` |
| Disk usage % | `statvfs()` on one configured filesystem |
| Swap usage % | `/proc/meminfo`; reported as unavailable when no swap is configured |
| Network upload / download | `/proc/net/dev` bytes per second for one interface |

It reports every interval, including when everything is healthy, because each report also acts as a heartbeat. Thresholds, incidents, offline detection and notifications are handled by LivePulser, not by the agent. The interval, disk path, network interface and thresholds are all managed in the dashboard.

## Design goals

- **Lightweight.** A single thread sleeps on absolute deadlines and wakes only to measure, send or refresh configuration. Memory is bounded. The agent never runs shell commands, `top`, `df` or `ip`, and it does no disk writes per report.
- **Outbound HTTPS only.** It opens no listening sockets and needs no inbound firewall rules. TLS verification is always on, redirects are never followed, and environment proxy variables are ignored unless a proxy is set explicitly.
- **Unprivileged.** It runs as a dedicated user and needs no Linux capabilities.
- **Honest data.** A reading the agent cannot measure reliably is sent as `null`, never as an invented zero.

See [docs/metrics.md](docs/metrics.md) for exact metric definitions and [docs/protocol.md](docs/protocol.md) for the wire protocol. Anyone can build a compatible agent from the protocol document.

## Requirements

- Linux on amd64 or arm64, measured from the host namespace. Container deployments are not supported yet.
- libcurl with TLS support, plus the system CA certificates.
- To build: a C17 compiler and make. Python 3 is needed only for the integration tests.

## Build

```sh
# Debian / Ubuntu
sudo apt install build-essential libcurl4-openssl-dev ca-certificates
# Fedora / RHEL
sudo dnf install gcc make libcurl-devel ca-certificates

make
make test        # unit tests and integration tests under ASan/UBSan
```

The binary is written to `build/livepulser-agent`.

## Install

1. Create a resource monitor in the LivePulser dashboard. Copy its monitor ID and API key; the key is shown only once.
2. Install the binary, the systemd unit and an example configuration:

   ```sh
   sudo make install
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin livepulser-agent
   ```

3. Store the API key so that only root and the agent can read it:

   ```sh
   sudo install -m 0640 -o root -g livepulser-agent /dev/null /etc/livepulser-agent/api-key
   sudo sh -c 'cat > /etc/livepulser-agent/api-key'   # paste the key, press Enter, then Ctrl-D
   sudo chgrp livepulser-agent /etc/livepulser-agent /etc/livepulser-agent/agent.conf
   ```

4. Set `monitor_id` in `/etc/livepulser-agent/agent.conf`, then verify the setup:

   ```sh
   sudo -u livepulser-agent livepulser-agent --config /etc/livepulser-agent/agent.conf --check
   ```

5. Start the service:

   ```sh
   sudo systemctl daemon-reload
   sudo systemctl enable --now livepulser-agent
   journalctl -u livepulser-agent -f
   ```

The first report arrives about one interval after start. Before that, the agent needs a full measurement window. Dashboard history fills in once each five-minute bucket closes.

[docs/installation.md](docs/installation.md) covers upgrades, uninstalling and running without systemd.

## Configuration

`/etc/livepulser-agent/agent.conf` uses `key=value` lines. Unknown keys are rejected so that typos cannot go unnoticed.

| Key | Required | Description |
| --- | --- | --- |
| `endpoint` | yes | API origin, normally `https://rs.livepulser.com` |
| `monitor_id` | yes | Resource monitor UUID from the dashboard |
| `api_key_file` | yes* | File containing the monitor API key. Must not be readable by other users. *Optional under systemd when the key is passed with `LoadCredential=api-key:...` |
| `state_dir` | no | Session state directory. Defaults to systemd's `$STATE_DIRECTORY`, then `/var/lib/livepulser-agent` |
| `ca_file` | no | CA bundle to use instead of the system default |
| `proxy` | no | Explicit HTTP(S) proxy URL. `http_proxy`/`https_proxy` environment variables are ignored |

Command line:

```
livepulser-agent [--config PATH] [--check] [--verbose]
```

| Exit code | Meaning |
| --- | --- |
| 0 | Clean shutdown, or `--check` succeeded |
| 1 | Runtime failure (systemd restarts the agent) |
| 77 | API key rejected at startup |
| 78 | Invalid configuration, unsupported server schema, or another instance holds the state lock |

The unit sets `RestartPreventExitStatus=77 78` so that a bad key or configuration cannot cause a restart loop.

## Rotating the API key

1. Generate a new key in the dashboard.
2. Replace the contents of the key file.
3. Run `sudo systemctl reload livepulser-agent` (this sends `SIGHUP`).

The agent keeps its session and sequence numbers across the reload. If the dashboard has not applied the new key yet, the agent logs that the key was rejected and stops sending. Reload again a few seconds later.

## Limitations

- Linux only, measuring the host namespace. Running it inside a container would report misleading numbers.
- One filesystem and one network interface per monitor. Network and FUSE filesystems are unsupported because `statvfs()` can block on them.
- With an empty `network_interface`, the agent uses the interface of the lowest-metric IPv4 default route, or the IPv6 default route when there is no IPv4 one. Only the main routing table is read. On hosts with policy routing or several equal default routes, set the interface explicitly in the dashboard.

## Security

See [SECURITY.md](SECURITY.md) for how to report vulnerabilities.

## License

[MIT](LICENSE). Bundles [cJSON](https://github.com/DaveGamble/cJSON) v1.7.19 (MIT), see [third_party/cjson](third_party/cjson).
