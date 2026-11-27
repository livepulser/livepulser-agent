#!/bin/sh
# Builds the Linux release archive inside an AlmaLinux 8 container (glibc 2.28), so the
# binary runs on every distribution with glibc 2.28 or later. It links the system
# libcurl, so the distribution's security updates cover TLS.
#
#   docker run --rm -v "$PWD:/src" -w /src almalinux:8 sh packaging/linux/build-release.sh VERSION ARCH
set -eu

version=$1
arch=$2
max_glibc=2.28

dnf install -y -q gcc make libcurl-devel binutils >/dev/null
make clean
make VERSION="$version"

# The highest glibc symbol version the binary asks for must not be above the target.
need=$(objdump -T build/livepulser-agent | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)
echo "highest glibc symbol version needed: $need"
if [ "$(printf '%s\n%s\n' "$need" "$max_glibc" | sort -V | tail -1)" != "$max_glibc" ]; then
	echo "the binary needs glibc $need, more than $max_glibc" >&2
	exit 1
fi

name="livepulser-agent-$version-linux-$arch"
mkdir -p "dist/$name"
strip -o "dist/$name/livepulser-agent" build/livepulser-agent
cp README.md CHANGELOG.md LICENSE packaging/agent.conf.example packaging/systemd/livepulser-agent.service "dist/$name/"
cp third_party/cjson/LICENSE "dist/$name/LICENSE.cjson"
tar -C dist -czf "dist/$name.tar.gz" "$name"
