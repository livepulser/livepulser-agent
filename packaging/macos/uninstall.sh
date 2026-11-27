#!/bin/sh
# Removes the launchd daemon and binary. Pass --purge to also delete configuration, state and the service user.
set -eu

[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
launchctl bootout system/com.livepulser.agent 2>/dev/null || true
rm -f /Library/LaunchDaemons/com.livepulser.agent.plist /usr/local/bin/livepulser-agent
if [ "${1:-}" = "--purge" ]; then
	rm -rf /usr/local/etc/livepulser-agent /usr/local/var/livepulser-agent
	dscl . -delete /Users/_livepulser 2>/dev/null || true
	dscl . -delete /Groups/_livepulser 2>/dev/null || true
fi
echo "Uninstalled."
