#!/bin/sh
# Runs a release binary on the system it's called on. The release workflow runs it in a
# container for each supported distribution: it installs libcurl as install.sh does,
# checks that every library resolves, and starts the binary.
#
#   docker run --rm -v "$PWD:/src:ro" -w /src IMAGE sh packaging/linux/smoke.sh path/to/livepulser-agent
set -eu

bin=$1
# shellcheck source=/dev/null
. /etc/os-release
echo "== $PRETTY_NAME, $(getconf GNU_LIBC_VERSION)"

if command -v apt-get >/dev/null 2>&1; then
	apt-get update -qq >/dev/null
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libcurl4 ca-certificates >/dev/null
fi

if command -v ldd >/dev/null 2>&1; then
	ldd "$bin"
	if ldd "$bin" | grep -q 'not found'; then
		echo "a library is missing" >&2
		exit 1
	fi
fi

"$bin" --version
