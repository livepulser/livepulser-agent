# Installation details

The [README](../README.md) covers the normal systemd installation. This page covers the remaining cases.

## Files

| Path | Owner / mode | Purpose |
| --- | --- | --- |
| `/usr/local/bin/livepulser-agent` | root, 0755 | Binary |
| `/etc/livepulser-agent/agent.conf` | root:livepulser-agent, 0640 | Local configuration |
| `/etc/livepulser-agent/api-key` | root:livepulser-agent, 0640 | Monitor API key |
| `/var/lib/livepulser-agent/` | livepulser-agent, 0700 | Session state and lock (created by systemd) |
| `/etc/systemd/system/livepulser-agent.service` | root, 0644 | Service unit |

The state directory holds two files:

- `state`: the session UUID and the highest reserved sequence number. It never contains the API key.
- `agent.lock`: an advisory lock that stops a second agent from using the same state.

Do not copy the state directory into machine images. Every server needs its own monitor and its own session.

## Passing the key as a systemd credential

As an alternative to a group-readable key file, keep the key root-only and let systemd pass it to the agent:

```ini
# systemctl edit livepulser-agent
[Service]
LoadCredential=api-key:/etc/livepulser-agent/api-key
```

Then remove `api_key_file` from `agent.conf`. The agent reads `$CREDENTIALS_DIRECTORY/api-key`.

## Running without systemd

The agent runs in the foreground and logs to stderr. Under any supervisor:

- run it as an unprivileged user;
- give it a private state directory (`state_dir=`) owned by that user with mode 0700;
- send `SIGTERM` to stop it and `SIGHUP` to reload the key;
- do not restart it automatically after exit codes 77 or 78.

## Hardening notes

The shipped unit uses a strict systemd sandbox. Some settings must not be added, because they would hide the host data the agent measures:

- `PrivateNetwork=`: the agent would see only an empty network namespace;
- `ProtectProc=invisible` combined with `ProcSubset=pid`: this hides `/proc/stat` and `/proc/meminfo`;
- `PrivateMounts=` or `TemporaryFileSystem=` over the measured disk path: the disk reading would describe the wrong filesystem.

TLS verification cannot be disabled. If certificate errors appear, install the distribution's `ca-certificates` package or point `ca_file=` at a valid bundle.

## Upgrading

```sh
git pull && make && sudo make install && sudo systemctl restart livepulser-agent
```

The session and sequence reservation survive the restart.

## Uninstalling

```sh
sudo systemctl disable --now livepulser-agent
sudo make uninstall
sudo rm -rf /etc/livepulser-agent /var/lib/livepulser-agent
sudo userdel livepulser-agent
```

Delete or pause the monitor in the dashboard as well. Otherwise it will report the server as offline.
