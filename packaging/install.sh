#!/bin/sh
# Installs or upgrades the LivePulser resource agent on Linux (systemd) or macOS (launchd).
#
#   curl -fsSL https://github.com/livepulser/livepulser-agent/releases/latest/download/install.sh |
#     sudo sh -s -- --monitor-id <monitor-id>
#
# The script asks for the monitor API key on the terminal, so the key never appears in
# the command line or the shell history. Without a terminal, pass --api-key-file.
#
# It downloads the release archive for this machine, checks it against SHA256SUMS,
# verifies the key and configuration with --check, and only then installs and
# (re)starts the service. If anything fails, the existing installation is unchanged.
# Running it again upgrades the agent and keeps the configuration and the key.
set -eu

VERSION="@VERSION@"
REPO="https://github.com/livepulser/livepulser-agent"
DEFAULT_ENDPOINT="https://rs.livepulser.com"

monitor_id=""
endpoint=""
key_file=""
tmp=""
tty_echo_off=0

say() { printf '==> %s\n' "$*"; }
die() {
	printf 'livepulser-agent install: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<EOF
Usage: install.sh [--monitor-id ID] [--api-key-file PATH] [--endpoint URL]

  --monitor-id ID       resource monitor ID from the dashboard (required for a new install)
  --api-key-file PATH   read the API key from PATH instead of asking for it
  --endpoint URL        API origin (default $DEFAULT_ENDPOINT)

Without --api-key-file the key is read from the terminal. On an upgrade, press
Enter to keep the current key.
EOF
}

cleanup() {
	if [ "$tty_echo_off" = 1 ]; then
		stty echo </dev/tty 2>/dev/null || true
	fi
	if [ -n "$tmp" ]; then
		rm -rf "$tmp"
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

while [ $# -gt 0 ]; do
	case $1 in
	--monitor-id) [ $# -ge 2 ] || die "--monitor-id needs a value"; monitor_id=$2; shift 2 ;;
	--monitor-id=*) monitor_id=${1#*=}; shift ;;
	--api-key-file) [ $# -ge 2 ] || die "--api-key-file needs a value"; key_file=$2; shift 2 ;;
	--api-key-file=*) key_file=${1#*=}; shift ;;
	--endpoint) [ $# -ge 2 ] || die "--endpoint needs a value"; endpoint=$2; shift 2 ;;
	--endpoint=*) endpoint=${1#*=}; shift ;;
	-h | --help) usage; exit 0 ;;
	*) usage >&2; die "unknown option: $1" ;;
	esac
done

case $VERSION in
@*) die "this is the unreleased template; download install.sh from a release instead" ;;
esac
[ "$(id -u)" -eq 0 ] || die "run as root, for example: curl -fsSL ... | sudo sh -s -- --monitor-id <monitor-id>"

if [ -n "$monitor_id" ] &&
	! printf '%s\n' "$monitor_id" | grep -Eq '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$'; then
	die "--monitor-id must be the monitor ID shown in the dashboard"
fi
if [ -n "$endpoint" ]; then
	case $endpoint in
	https://*) ;;
	*) die "--endpoint must start with https://" ;;
	esac
fi

# --- Platform ---------------------------------------------------------------

os=$(uname -s)
arch=$(uname -m)
case $os in
Linux)
	case $arch in
	x86_64 | amd64) asset="livepulser-agent-$VERSION-linux-amd64" ;;
	aarch64 | arm64) asset="livepulser-agent-$VERSION-linux-arm64" ;;
	*) die "no release build for $arch; build from source: $REPO#building" ;;
	esac
	[ -d /run/systemd/system ] || die "systemd is not running; see $REPO/blob/main/docs/installation.md#running-without-a-service-manager"
	glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}') || glibc=""
	[ -n "$glibc" ] || die "the release build needs glibc (musl distributions such as Alpine: build from source)"
	major=${glibc%%.*}
	minor=${glibc#*.}
	minor=${minor%%.*}
	if [ "$major" -lt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -lt 35 ]; }; then
		die "glibc $glibc is too old; the release build needs 2.35 or later (Ubuntu 22.04, Debian 12). Build from source: $REPO#building"
	fi
	user=livepulser-agent
	group=livepulser-agent
	etc=/etc/livepulser-agent
	;;
Darwin)
	asset="livepulser-agent-$VERSION-macos-universal"
	user=_livepulser
	group=_livepulser
	etc=/usr/local/etc/livepulser-agent
	;;
*) die "unsupported system $os; Windows uses install.ps1 ($REPO#install)" ;;
esac
conf=$etc/agent.conf
keyf=$etc/api-key
bin=/usr/local/bin/livepulser-agent

# --- Configuration and key ---------------------------------------------------

conf_value() { # key: print its value from the installed agent.conf
	[ -f "$conf" ] || return 0
	sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$conf" | tail -n 1
}
installed_id=$(conf_value monitor_id)
installed_endpoint=$(conf_value endpoint)
case $installed_id in 00000000-0000-4000-8000-000000000000) installed_id="" ;; esac
[ -n "$monitor_id" ] || monitor_id=$installed_id
[ -n "$monitor_id" ] || die "--monitor-id is required for a new installation"
[ -n "$endpoint" ] || endpoint=${installed_endpoint:-$DEFAULT_ENDPOINT}

# The current key is only worth keeping for the same monitor.
keep_key=0
if [ -s "$keyf" ] && [ "$monitor_id" = "$installed_id" ]; then
	keep_key=1
fi

key=""
if [ -n "$key_file" ]; then
	[ -r "$key_file" ] || die "cannot read $key_file"
	key=$(tr -d ' \r\n' <"$key_file")
elif (: </dev/tty) 2>/dev/null; then
	if [ "$keep_key" = 1 ]; then
		printf 'Resource monitor API key (press Enter to keep the current key): ' >/dev/tty
	else
		printf 'Resource monitor API key: ' >/dev/tty
	fi
	if stty -echo </dev/tty 2>/dev/null; then
		tty_echo_off=1
	fi
	IFS= read -r key </dev/tty || key=""
	if [ "$tty_echo_off" = 1 ]; then
		stty echo </dev/tty 2>/dev/null || true
		tty_echo_off=0
	fi
	printf '\n' >/dev/tty
	key=$(printf '%s' "$key" | tr -d ' \r\n')
fi
if [ -z "$key" ]; then
	[ "$keep_key" = 1 ] || die "the API key is required (no terminal to ask on: pass --api-key-file)"
	say "Keeping the current API key"
elif ! printf '%s\n' "$key" | grep -Eq '^[A-Za-z0-9_-]{43}$'; then
	die "that doesn't look like a resource monitor API key (43 characters, shown once in the dashboard)"
fi

# --- Download and verify -----------------------------------------------------

tmp=$(mktemp -d "${TMPDIR:-/tmp}/livepulser-agent.XXXXXX")

fetch() { # url dest
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$2" "$1"
	elif command -v wget >/dev/null 2>&1; then
		wget -q --https-only -O "$2" "$1"
	else
		die "curl or wget is needed to download the agent"
	fi
}

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

base="$REPO/releases/download/v$VERSION"
say "Downloading livepulser-agent $VERSION ($asset)"
fetch "$base/$asset.tar.gz" "$tmp/$asset.tar.gz" || die "download failed: $base/$asset.tar.gz"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/SHA256SUMS"
expected=$(awk -v f="$asset.tar.gz" '$2 == f || $2 == "*" f {print $1}' "$tmp/SHA256SUMS")
[ -n "$expected" ] || die "$asset.tar.gz is not listed in SHA256SUMS"
[ "$(sha256 "$tmp/$asset.tar.gz")" = "$expected" ] || die "checksum mismatch for $asset.tar.gz; nothing was installed"
tar -xzf "$tmp/$asset.tar.gz" -C "$tmp"
src=$tmp/$asset
[ -x "$src/livepulser-agent" ] || die "the archive has no livepulser-agent binary"

# --- Dependencies and the service account ------------------------------------

if [ "$os" = Linux ] && ldd "$src/livepulser-agent" 2>/dev/null | grep -q 'libcurl.*not found'; then
	say "Installing libcurl"
	if command -v apt-get >/dev/null 2>&1; then
		DEBIAN_FRONTEND=noninteractive apt-get install -y -q libcurl4 ca-certificates >/dev/null 2>&1 ||
			{ apt-get update -q >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -q libcurl4 ca-certificates >/dev/null; } ||
			die "could not install libcurl; run: apt-get install libcurl4 ca-certificates"
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -y -q libcurl ca-certificates >/dev/null || die "could not install libcurl; run: dnf install libcurl ca-certificates"
	elif command -v yum >/dev/null 2>&1; then
		yum install -y -q libcurl ca-certificates >/dev/null || die "could not install libcurl; run: yum install libcurl ca-certificates"
	elif command -v zypper >/dev/null 2>&1; then
		zypper --non-interactive --quiet install libcurl4 ca-certificates >/dev/null || die "could not install libcurl; run: zypper install libcurl4 ca-certificates"
	else
		die "libcurl is missing; install it with your package manager and run this again"
	fi
fi

create_user_linux() {
	id -u "$user" >/dev/null 2>&1 && return 0
	nologin=$(command -v nologin 2>/dev/null || true)
	[ -n "$nologin" ] || nologin=/bin/false
	say "Creating the $user system user"
	useradd --system --no-create-home --shell "$nologin" "$user"
}

create_user_macos() {
	dscl . -read "/Users/$user" >/dev/null 2>&1 && return 0
	say "Creating the hidden $user user"
	id=300
	while dscl . -list /Users UniqueID | awk '{print $2}' | grep -qx "$id" ||
		dscl . -list /Groups PrimaryGroupID | awk '{print $2}' | grep -qx "$id"; do
		id=$((id + 1))
	done
	dscl . -create "/Groups/$group" PrimaryGroupID "$id"
	dscl . -create "/Groups/$group" RealName "LivePulser agent"
	dscl . -create "/Users/$user" UniqueID "$id"
	dscl . -create "/Users/$user" PrimaryGroupID "$id"
	dscl . -create "/Users/$user" UserShell /usr/bin/false
	dscl . -create "/Users/$user" NFSHomeDirectory /var/empty
	dscl . -create "/Users/$user" RealName "LivePulser agent"
	dscl . -create "/Users/$user" Password '*'
	dscl . -create "/Users/$user" IsHidden 1
}

if [ "$os" = Linux ]; then create_user_linux; else create_user_macos; fi

# --- Stage the configuration and check it with the new binary ----------------
# The staged files live in a directory only root and the service group can read,
# so --check runs exactly as the service will, before anything is replaced.

stage=$tmp/stage
mkdir -m 0750 "$stage"
chgrp "$group" "$tmp" "$stage"
chmod 0750 "$tmp"
install -m 0755 "$src/livepulser-agent" "$stage/livepulser-agent"

if [ -n "$key" ]; then
	(umask 077 && printf '%s\n' "$key" >"$stage/api-key")
	chgrp "$group" "$stage/api-key"
	chmod 0640 "$stage/api-key"
	key=""
	check_key=$stage/api-key
else
	check_key=$keyf
fi

write_conf() { # api_key_file > config on stdout
	if [ -f "$conf" ]; then
		awk -v id="$monitor_id" -v ep="$endpoint" -v kf="$1" '
			/^[[:space:]]*monitor_id[[:space:]]*=/ { print "monitor_id=" id; seen_id = 1; next }
			/^[[:space:]]*endpoint[[:space:]]*=/ { print "endpoint=" ep; seen_ep = 1; next }
			/^[[:space:]]*api_key_file[[:space:]]*=/ { print "api_key_file=" kf; seen_kf = 1; next }
			{ print }
			END {
				if (!seen_ep) print "endpoint=" ep
				if (!seen_id) print "monitor_id=" id
				if (!seen_kf) print "api_key_file=" kf
			}' "$conf"
	else
		printf '# LivePulser resource agent configuration.\n'
		printf '# Everything else (interval, disk path, network interface, thresholds) is managed in the dashboard.\n\n'
		printf 'endpoint=%s\nmonitor_id=%s\napi_key_file=%s\n' "$endpoint" "$monitor_id" "$1"
	fi
}
(umask 027 && write_conf "$check_key" >"$stage/check.conf" && write_conf "$keyf" >"$stage/agent.conf")
chgrp "$group" "$stage/check.conf" "$stage/agent.conf"

as_service_user() {
	if command -v runuser >/dev/null 2>&1; then
		runuser -u "$user" -- "$@"
	else
		sudo -u "$user" "$@"
	fi
}

say "Checking the key and configuration"
set +e
as_service_user "$stage/livepulser-agent" --config "$stage/check.conf" --check >"$tmp/check.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
	sed 's/^/    /' "$tmp/check.out" >&2
	case $rc in
	77) die "the API key was rejected. Check the monitor ID, or generate a new key in the dashboard. Nothing was changed." ;;
	78) die "the configuration is invalid (exit code 78). Nothing was changed." ;;
	126) die "cannot run the agent from $tmp (is ${TMPDIR:-/tmp} mounted noexec? set TMPDIR to another directory). Nothing was changed." ;;
	*) die "the check failed (exit code $rc). Nothing was changed." ;;
	esac
fi

# --- Install -----------------------------------------------------------------

say "Installing livepulser-agent $VERSION"
install -d -m 0755 /usr/local/bin
# Replace the binary by rename, so a running agent is never left with a half-written file.
install -m 0755 "$stage/livepulser-agent" "$bin.new"
mv -f "$bin.new" "$bin"
install -d -m 0750 -o root -g "$group" "$etc"
install -m 0640 -o root -g "$group" "$stage/agent.conf" "$conf"
if [ -f "$stage/api-key" ]; then
	install -m 0640 -o root -g "$group" "$stage/api-key" "$keyf"
fi

if [ "$os" = Linux ]; then
	install -m 0644 "$src/livepulser-agent.service" /etc/systemd/system/livepulser-agent.service
	systemctl daemon-reload
	systemctl enable --quiet livepulser-agent
	systemctl restart livepulser-agent
	logs="journalctl -u livepulser-agent -f"
else
	var=/usr/local/var/livepulser-agent
	plist=/Library/LaunchDaemons/com.livepulser.agent.plist
	install -d -m 0700 -o "$user" -g "$group" "$var"
	if launchctl print system/com.livepulser.agent >/dev/null 2>&1; then
		if cmp -s "$src/com.livepulser.agent.plist" "$plist"; then
			launchctl kickstart -k system/com.livepulser.agent
		else
			launchctl bootout system/com.livepulser.agent 2>/dev/null || true
			install -m 0644 -o root -g wheel "$src/com.livepulser.agent.plist" "$plist"
			n=0
			until launchctl bootstrap system "$plist" 2>/dev/null; do
				n=$((n + 1))
				[ "$n" -lt 10 ] || die "launchctl bootstrap failed; run: sudo launchctl bootstrap system $plist"
				sleep 1
			done
		fi
	else
		install -m 0644 -o root -g wheel "$src/com.livepulser.agent.plist" "$plist"
		launchctl bootstrap system "$plist"
	fi
	logs="tail -f $var/agent.log"
fi

say "livepulser-agent $VERSION is running for monitor $monitor_id"
printf '    The dashboard shows the server about one upload interval from now.\n'
printf '    Logs: %s\n' "$logs"
