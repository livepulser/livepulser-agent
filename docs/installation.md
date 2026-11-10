# Installation details

The [README](../README.md) covers the normal installation on each platform. This page lists the installed files and covers upgrades, uninstalling and running without a service manager.

## Installed files

### Linux

| Path | Owner / mode | Purpose |
| --- | --- | --- |
| `/usr/local/bin/livepulser-agent` | root, 0755 | Binary |
| `/etc/livepulser-agent/agent.conf` | root:livepulser-agent, 0640 | Local configuration |
| `/etc/livepulser-agent/api-key` | root:livepulser-agent, 0640 | Monitor API key |
| `/var/lib/livepulser-agent/` | livepulser-agent, 0700 | Session state and lock (created by systemd) |
| `/etc/systemd/system/livepulser-agent.service` | root, 0644 | Service unit |

### macOS

| Path | Owner / mode | Purpose |
| --- | --- | --- |
| `/usr/local/bin/livepulser-agent` | root, 0755 | Binary |
| `/usr/local/etc/livepulser-agent/agent.conf` | root:_livepulser, 0640 | Local configuration |
| `/usr/local/etc/livepulser-agent/api-key` | root:_livepulser, 0640 | Monitor API key |
| `/usr/local/var/livepulser-agent/` | _livepulser, 0700 | Session state, lock and `agent.log` |
| `/Library/LaunchDaemons/com.livepulser.agent.plist` | root, 0644 | launchd daemon |

### Windows

| Path | Access | Purpose |
| --- | --- | --- |
| `C:\Program Files\LivePulser Agent\livepulser-agent.exe` | default | Binary |
| `C:\ProgramData\LivePulser\agent.conf` | Administrators, SYSTEM; service: read | Local configuration |
| `C:\ProgramData\LivePulser\api-key` | Administrators, SYSTEM; service: read | Monitor API key |
| `C:\ProgramData\LivePulser\state\` | Administrators, SYSTEM; service: modify | Session state, lock and `agent.log` |
| `LivePulserAgent` service | `NT SERVICE\LivePulserAgent` | Delayed automatic start; restarts after a crash |

## State and logs

The state directory holds two files:

- `state`: the session UUID and the highest reserved sequence number. It never contains the API key.
- `agent.lock`: a lock that stops a second agent from using the same state.

Do not copy the state directory into machine images. Every server needs its own monitor and its own session.

Log files written with `--log-file` (and by the Windows service) are rotated to `agent.log.1` at 1 MiB, so at most 2 MiB is used.

## Platform notes

### Linux: key as a systemd credential

As an alternative to a group-readable key file, keep the key root-only and let systemd pass it to the agent:

```ini
# systemctl edit livepulser-agent
[Service]
LoadCredential=api-key:/etc/livepulser-agent/api-key
```

Then remove `api_key_file` from `agent.conf`. The agent reads `$CREDENTIALS_DIRECTORY/api-key`.

### Linux: hardening

The shipped unit uses a strict systemd sandbox. Some settings must not be added, because they would hide the host data the agent measures:

- `PrivateNetwork=`: the agent would see only an empty network namespace;
- `ProtectProc=invisible` combined with `ProcSubset=pid`: this hides `/proc/stat` and `/proc/meminfo`;
- `PrivateMounts=` or `TemporaryFileSystem=` over the measured disk path: the disk reading would describe the wrong filesystem.

### macOS

- **Restarts.** launchd restarts the agent after any non-zero exit, at most once per minute (`ThrottleInterval`). A rejected key at startup therefore costs one configuration request per minute until it is fixed. Watch `agent.log` after installing.
- **Unsigned binaries.** Release binaries are not notarized. A binary downloaded with a web browser is quarantined; remove the quarantine with `xattr -d com.apple.quarantine livepulser-agent`, or build from source.

### Windows

- **Restarts.** The service restarts after a crash. It stops (and stays stopped) when the key is rejected at startup or the configuration is invalid. The exit code is visible with `sc.exe query LivePulserAgent` as a service-specific error (77 or 78).
- **Running in a console.** The agent can run in a console for troubleshooting. Use an elevated prompt so it can read the protected key file: `livepulser-agent.exe --config C:\ProgramData\LivePulser\agent.conf --verbose`. Ctrl+C stops it.
- **Proxies and certificates.** TLS uses the Windows certificate store. Proxies configured in Windows or WinHTTP are ignored unless `proxy=` is set.

TLS verification cannot be disabled on any platform. If certificate errors appear, update the system's root certificates (`ca-certificates` on Linux).

## Running without a service manager

The agent runs in the foreground. Under any supervisor:

- run it as an unprivileged user;
- give it a private state directory (`state_dir=`) owned by that user;
- send `SIGTERM` (Ctrl+C on Windows) to stop it, and `SIGHUP` to reload the key;
- do not restart it automatically after exit codes 77 or 78.

## Upgrading

| Platform | Steps |
| --- | --- |
| Linux | `make && sudo make install && sudo systemctl restart livepulser-agent` |
| macOS | Run `install.sh` with the new binary, then `sudo launchctl kickstart -k system/com.livepulser.agent` |
| Windows | Run `install.ps1` again from the new release. It stops the service, replaces the binary and starts it again |

The session and sequence reservation survive the upgrade.

## Uninstalling

| Platform | Steps |
| --- | --- |
| Linux | `sudo systemctl disable --now livepulser-agent && sudo make uninstall`, then remove `/etc/livepulser-agent`, `/var/lib/livepulser-agent` and the user |
| macOS | `sudo ./uninstall.sh --purge` |
| Windows | `.\uninstall.ps1 -Purge` from an elevated PowerShell prompt |

Delete or pause the monitor in the dashboard as well. Otherwise it will report the server as offline.
