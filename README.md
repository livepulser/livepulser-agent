# livepulser-agent

A small, auditable C agent that reports server resource usage to [LivePulser](https://livepulser.com) resource monitors. It runs on **Linux, macOS and Windows**.

The agent measures six values every few seconds and uploads them in batches (every 60 seconds by default):

| Metric | Linux | macOS | Windows |
| --- | --- | --- | --- |
| CPU usage % | `/proc/stat` | `host_processor_info` | `GetSystemTimes` |
| Memory usage % | `MemTotal − MemAvailable` | app + wired + compressed memory | `GlobalMemoryStatusEx` |
| Disk usage % | `statvfs()` | `statfs()` (APFS container) | `GetDiskFreeSpaceExW` |
| Swap usage % | `/proc/meminfo` | `vm.swapusage` | paging file (`\Paging File(_Total)\% Usage`) |
| Network upload / download | `/proc/net/dev` | interface counters (`NET_RT_IFLIST2`) | `GetIfEntry2` |

The agent uploads on schedule even when everything is healthy, because each upload also acts as a heartbeat. It also checks thresholds locally and uploads immediately when a breach starts or ends, so alerts open within the breach time you choose instead of waiting for the next upload. LivePulser re-checks every sample and owns incidents, offline detection and notifications. Thresholds, alert timing, the upload interval, disk path and network interface are all managed in the dashboard.

## Alert timing

Each monitor has three settings in the dashboard:

| Setting | Default | Meaning |
| --- | --- | --- |
| Alert after | 30 s | How long a metric must stay at or above its threshold before an alert opens |
| Recover after | 60 s | How long the metrics that caused the alert must stay below their recovery level before it closes |
| Recovery margin | 0 | How far below the threshold the recovery level sits (percentage points; percent of the threshold for network) |

Only the metrics that caused an alert decide when it recovers, and the dashboard and notifications name them.

## Design goals

- **Lightweight.** A single thread sleeps until the next deadline and wakes only to measure, upload or refresh configuration. Configuration changes arrive with upload responses, so there is no polling. Memory use is bounded and nothing is written to disk per report. The agent never runs shell commands or helper tools.
- **No extra runtime dependencies.** On Linux it uses the system libcurl. On macOS it uses the libcurl that ships with the OS. On Windows it uses WinHTTP, which is built into the system.
- **Outbound HTTPS only.** The agent opens no listening sockets. TLS verification is always on and redirects are never followed. System and environment proxy settings are ignored unless a proxy is configured explicitly.
- **Unprivileged.** It runs under a dedicated account with no administrator rights.
- **Honest data.** A reading that cannot be measured reliably is sent as `null`, never as an invented zero.

Exact metric definitions are in [docs/metrics.md](docs/metrics.md). The wire protocol is described in [docs/protocol.md](docs/protocol.md), so anyone can build a compatible agent.

## Supported platforms

| Platform | Architectures | Service integration |
| --- | --- | --- |
| Linux | amd64, arm64 | systemd unit |
| macOS 11 or later | Apple silicon, Intel (universal binary) | launchd daemon |
| Windows 10 / Server 2016 or later | amd64 | Windows service |

The agent measures the host it runs on. Running it inside a container is not supported.

## Install

Create a resource monitor in the LivePulser dashboard, then copy its **monitor ID** and **API key**. The key is shown only once. The dashboard shows these commands with the monitor ID filled in.

### Quick install

Linux (systemd, amd64 or arm64) and macOS 11 or later:

```sh
curl -fsSL https://github.com/livepulser/livepulser-agent/releases/latest/download/install.sh | sudo sh -s -- --monitor-id <monitor-id>
```

Windows 10 / Server 2016 or later, from an elevated PowerShell prompt:

```powershell
& ([scriptblock]::Create((irm https://github.com/livepulser/livepulser-agent/releases/latest/download/install.ps1))) -MonitorId <monitor-id>
```

The installer asks for the API key, so it never appears in the command line or the shell history. It downloads the release for the machine and checks it against `SHA256SUMS`. It sets up the service account, configuration and key, and verifies them with `--check` before it installs or starts anything. If any step fails, an existing installation is left as it was.

Run the same command again to upgrade, and press Enter at the key prompt to keep the current key. For an unattended install on Linux or macOS, pass `--api-key-file PATH` instead of typing the key.

To read the installer before running it, download it first (`curl -fsSLO …/install.sh`). Each release lists its checksum in `SHA256SUMS`.

## Manual install

### Linux

Download the archive for your architecture from the [releases page](https://github.com/livepulser/livepulser-agent/releases) and check it against `SHA256SUMS`. The binary needs libcurl and glibc 2.35 or later (Ubuntu 22.04, Debian 12 or newer; on other distributions, build from source):

```sh
sudo apt install libcurl4 ca-certificates          # or: dnf install libcurl ca-certificates
tar -xzf livepulser-agent-1.0.0-linux-amd64.tar.gz
cd livepulser-agent-1.0.0-linux-amd64
sudo install -m 0755 livepulser-agent /usr/local/bin/livepulser-agent
sudo install -d -m 0750 /etc/livepulser-agent
sudo install -m 0640 agent.conf.example /etc/livepulser-agent/agent.conf
sudo install -m 0644 livepulser-agent.service /etc/systemd/system/livepulser-agent.service
```

Or build from source instead:

```sh
sudo apt install build-essential libcurl4-openssl-dev ca-certificates   # or: dnf install gcc make libcurl-devel
make && sudo make install
```

Then create the service user, store the key and start the agent:

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin livepulser-agent
sudo install -m 0640 -o root -g livepulser-agent /dev/null /etc/livepulser-agent/api-key
sudo sh -c 'cat > /etc/livepulser-agent/api-key'    # paste the key, press Enter, then Ctrl-D
sudo chgrp livepulser-agent /etc/livepulser-agent /etc/livepulser-agent/agent.conf
sudoedit /etc/livepulser-agent/agent.conf             # set monitor_id
sudo -u livepulser-agent livepulser-agent --config /etc/livepulser-agent/agent.conf --check
sudo systemctl daemon-reload && sudo systemctl enable --now livepulser-agent
```

To follow the logs, run `journalctl -u livepulser-agent -f`.

### macOS

Use the release archive, or build with `make` (this needs the Xcode command line tools). Then run:

```sh
sudo ./install.sh ./livepulser-agent      # packaging/macos/install.sh when building from source
sudo sh -c 'cat > /usr/local/etc/livepulser-agent/api-key'
sudo nano /usr/local/etc/livepulser-agent/agent.conf    # set monitor_id
sudo -u _livepulser /usr/local/bin/livepulser-agent --config /usr/local/etc/livepulser-agent/agent.conf --check
sudo launchctl bootstrap system /Library/LaunchDaemons/com.livepulser.agent.plist
```

The agent runs as the hidden `_livepulser` user. Logs go to `/usr/local/var/livepulser-agent/agent.log`.

### Windows

Extract the release zip, then run this from an elevated PowerShell prompt:

```powershell
.\install.ps1 -MonitorId 00000000-0000-4000-8000-000000000000
```

The script does the following:

- prompts for the API key;
- installs the agent to `C:\Program Files\LivePulser Agent`;
- writes its configuration and key to `C:\ProgramData\LivePulser`, readable only by Administrators, SYSTEM and the service;
- verifies the setup with `--check`;
- starts the `LivePulserAgent` service under the `NT SERVICE\LivePulserAgent` virtual account.

Logs go to `C:\ProgramData\LivePulser\state\agent.log`.

On all platforms the first report arrives about one interval after start, because the agent needs one full measurement window. Dashboard history fills in once each five-minute bucket closes. [docs/installation.md](docs/installation.md) covers upgrades, uninstalling and running without a service manager.

## Configuration

The configuration file uses `key=value` lines. Unknown keys are rejected so that typos cannot go unnoticed.

| Key | Required | Description |
| --- | --- | --- |
| `endpoint` | yes | API origin, normally `https://rs.livepulser.com` |
| `monitor_id` | yes | Resource monitor UUID from the dashboard |
| `api_key_file` | yes* | File containing the monitor API key. It must not be readable by other users. *Optional under systemd when the key is passed with `LoadCredential=api-key:...` |
| `state_dir` | no | Session state directory. The default depends on the platform (see below) |
| `ca_file` | no | CA bundle to use instead of the system trust store. Linux and macOS only; on Windows, add the CA to the certificate store instead |
| `proxy` | no | Explicit HTTP proxy URL. System and environment proxy settings are ignored |

| Platform | Default configuration | Default state directory |
| --- | --- | --- |
| Linux | `/etc/livepulser-agent/agent.conf` | `$STATE_DIRECTORY` or `/var/lib/livepulser-agent` |
| macOS | `/usr/local/etc/livepulser-agent/agent.conf` | `/usr/local/var/livepulser-agent` |
| Windows | `C:\ProgramData\LivePulser\agent.conf` | `C:\ProgramData\LivePulser\state` |

On Windows, the dashboard disk path `/` means the system drive. A drive path such as `D:\` or `D:/data` selects another volume.

Command line:

```
livepulser-agent [--config PATH] [--check] [--log-file PATH] [--verbose]
```

`--service` is used only by the Windows service manager.

| Exit code | Meaning |
| --- | --- |
| 0 | Clean shutdown, or `--check` succeeded |
| 1 | Runtime failure |
| 77 | API key rejected at startup |
| 78 | Invalid configuration, unsupported server schema, or another instance holds the state lock |

## Rotating the API key

1. Generate a new key in the dashboard.
2. Replace the contents of the key file.
3. Tell the agent to reload it:

| Platform | Reload command |
| --- | --- |
| Linux | `sudo systemctl reload livepulser-agent` |
| macOS | `sudo launchctl kill HUP system/com.livepulser.agent` |
| Windows | `sc.exe control LivePulserAgent paramchange` |

The agent keeps its session and sequence numbers across the reload. If the dashboard has not applied the new key yet, the agent logs that the key was rejected and stops sending. Reload again a few seconds later.

## Limitations

- **One filesystem and one interface.** Each monitor measures one filesystem and one network interface. Network filesystems are unsupported because they can block.
- **Default interface.** With an empty `network_interface`, the agent picks the interface of the primary default route. If several default routes are equally preferred, set the interface in the dashboard. On Linux only the main routing table is read.
- **APFS disks.** On macOS, disk usage counts the whole APFS container, because all volumes in a container share its free space.

## Building

| Target | Command |
| --- | --- |
| Linux / macOS | `make` |
| Windows (cross-compiled with MinGW-w64) | `make CC=x86_64-w64-mingw32-gcc` |
| macOS universal binary | `make ARCHFLAGS="-arch arm64 -arch x86_64"` |
| Tests | `make test` (unit and integration tests; ASan/UBSan on Linux and macOS) |

## Security

See [SECURITY.md](SECURITY.md) for how to report vulnerabilities.

## License

[MIT](LICENSE). Bundles [cJSON](https://github.com/DaveGamble/cJSON) v1.7.19 (MIT), see [third_party/cjson](third_party/cjson).
