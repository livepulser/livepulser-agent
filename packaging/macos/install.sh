#!/bin/sh
# Installs livepulser-agent as a launchd daemon running as the hidden _livepulser user.
# Usage: sudo ./install.sh [path/to/livepulser-agent]
set -eu

BIN=${1:-./livepulser-agent}
HERE=$(cd "$(dirname "$0")" && pwd)
PLIST_SRC="$HERE/com.livepulser.agent.plist"
CONF_SRC="$HERE/agent.conf.example"
[ -f "$CONF_SRC" ] || CONF_SRC="$HERE/../agent.conf.example"
ETC=/usr/local/etc/livepulser-agent
VAR=/usr/local/var/livepulser-agent
PLIST=/Library/LaunchDaemons/com.livepulser.agent.plist
NAME=_livepulser

[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 1; }

if ! dscl . -read "/Users/$NAME" >/dev/null 2>&1; then
	id=300
	while dscl . -list /Users UniqueID | awk '{print $2}' | grep -qx "$id" ||
		dscl . -list /Groups PrimaryGroupID | awk '{print $2}' | grep -qx "$id"; do
		id=$((id + 1))
	done
	dscl . -create "/Groups/$NAME" PrimaryGroupID "$id"
	dscl . -create "/Groups/$NAME" RealName "LivePulser agent"
	dscl . -create "/Users/$NAME" UniqueID "$id"
	dscl . -create "/Users/$NAME" PrimaryGroupID "$id"
	dscl . -create "/Users/$NAME" UserShell /usr/bin/false
	dscl . -create "/Users/$NAME" NFSHomeDirectory /var/empty
	dscl . -create "/Users/$NAME" RealName "LivePulser agent"
	dscl . -create "/Users/$NAME" Password '*'
	dscl . -create "/Users/$NAME" IsHidden 1
fi

install -d -m 0755 /usr/local/bin
install -m 0755 "$BIN" /usr/local/bin/livepulser-agent
install -d -m 0750 -o root -g "$NAME" "$ETC"
[ -e "$ETC/agent.conf" ] || install -m 0640 -o root -g "$NAME" "$CONF_SRC" "$ETC/agent.conf"
[ -e "$ETC/api-key" ] || install -m 0640 -o root -g "$NAME" /dev/null "$ETC/api-key"
install -d -m 0700 -o "$NAME" -g "$NAME" "$VAR"
install -m 0644 -o root -g wheel "$PLIST_SRC" "$PLIST"

echo "Installed. Next steps:"
echo "  1. Put the monitor API key in $ETC/api-key and set monitor_id in $ETC/agent.conf"
echo "  2. sudo -u $NAME /usr/local/bin/livepulser-agent --config $ETC/agent.conf --check"
echo "  3. sudo launchctl bootstrap system $PLIST"
