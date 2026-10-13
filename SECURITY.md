# Security policy

## Reporting a vulnerability

Please do not open a public issue for security problems. Report them privately through [GitHub security advisories](https://github.com/livepulser/livepulser-agent/security/advisories/new). Include the affected version, the impact, and steps to reproduce. You will receive an acknowledgement within three business days.

Only the latest release receives security fixes.

## Threat model

The agent runs on customer servers and holds one secret, the monitor API key. Its design choices:

- **Key handling.** The key is read from a file that must not be accessible to other users. It is never placed in arguments, URLs, logs or the state file. In-memory copies are wiped on shutdown and on key reload.
- **Network exposure.** The agent opens only outbound HTTPS connections to the configured origin. It has no listening socket, no remote command execution and no self-update. Configuration from the server can never change the origin, the TLS settings or the files the agent reads.
- **TLS.** Peer and host verification are always on. Redirects are never followed, so the key cannot be forwarded to another host. Environment proxy variables are ignored unless a proxy is configured explicitly.
- **Input handling.** Server responses are size-limited (16 KiB body, bounded headers, JSON nesting depth 16) and validated before use. procfs is read line by line with fixed buffers.
- **Privileges.** The agent needs no root privileges or Linux capabilities. The systemd unit applies a strict sandbox.

The only third-party code is libcurl, used through the system package, and the vendored [cJSON](third_party/cjson) v1.7.19.
