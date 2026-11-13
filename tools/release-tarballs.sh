#!/bin/sh
# release-tarballs.sh - an FHS tarball per distro, each built in a throwaway copy and published only if
# that distro's gate passes.  Usage: tools/release-tarballs.sh [outdir] [distro ...]
set -u

SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-/var/tmp/pcrel}
shift 2>/dev/null || true
VER=$(sed -n 's/.*PC_VERSION[^"]*"\([^"]*\)".*/\1/p' "$SRC/src/version.h")
# .git is excluded from the throwaway copy, so the revision is passed in: a release binary must say
# which revision it is
REV=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)

# the platform must be EXPLICIT: podman serves whatever variant of a tag is in local storage, and
# qemu binfmt would run a wrong-arch binary through the gate unnoticed
PLATFORM=${PLATFORM:-linux/amd64}
case "$PLATFORM" in
linux/amd64)  WANT_ELF='x86-64';        ARCH=x86_64 ;;
linux/arm64)  WANT_ELF='ARM aarch64';   ARCH=aarch64 ;;
linux/arm/v7) WANT_ELF='ARM, EABI5';    ARCH=armv7 ;;
linux/386)    WANT_ELF='Intel 80386';   ARCH=i386 ;;
*) echo "unknown PLATFORM $PLATFORM"; exit 1 ;;
esac

# image:tag  pkg-manager-line, fully qualified (no unqualified-search registries).  Rocky needs EPEL
# for libsodium-devel; alpine needs linux-headers for <linux/futex.h> (packaging, not musl).
DISTROS='
debian:docker.io/library/debian:trixie-slim:apt-get update -qq && apt-get install -y -qq build-essential libsodium-dev libcurl4-openssl-dev pkg-config
ubuntu:docker.io/library/ubuntu:24.04:apt-get update -qq && apt-get install -y -qq build-essential libsodium-dev libcurl4-openssl-dev pkg-config
rocky:docker.io/rockylinux/rockylinux:9:dnf install -y -q epel-release && dnf install -y -q gcc make libsodium-devel libcurl-devel pkgconf
alpine:docker.io/library/alpine:3.20:apk add --no-cache build-base linux-headers libsodium-dev curl-dev pkgconf
'

# RUNTIME, else whichever of podman, nerdctl, docker is present
RT=${RUNTIME:-}
[ -n "$RT" ] || for c in podman nerdctl docker; do
	command -v "$c" >/dev/null 2>&1 && { RT=$c; break; }
done
[ -n "$RT" ] || { echo "no container runtime (podman, nerdctl or docker)"; exit 1; }
echo "runtime: $RT"
mkdir -p "$OUT"
echo "perfcached $VER, $PLATFORM ($ARCH) -> $OUT"

want() {
	[ $# -eq 0 ] && return 0
	for w in "$@"; do [ "$w" = "$1" ] && return 0; done
	return 1
}

pass=0; fail=0
echo "$DISTROS" | while IFS= read -r line; do
	[ -z "$line" ] && continue
	# name:image:tag:deps - image contains slashes and deps colons, so peel name off the front and
	# deps off the back at the FIRST ':' after the tag
	name=${line%%:*}; rest=${line#*:}
	imgtag=${rest%%:*}:${rest#*:}
	imgtag=${imgtag%%:apt-get*}; imgtag=${imgtag%%:dnf*}; imgtag=${imgtag%%:apk*}
	deps=${rest#"${imgtag}":}
	img=$imgtag

	work="$OUT/.build-$name"
	rm -rf "$work"; mkdir -p "$work"
	# throwaway copy: never let objects cross a libc
	tar -C "$SRC" --exclude=.git --exclude='*.o' --exclude='*.a' -cf - . \
		| tar -C "$work" -xf -

	# NOTHING but commands inside the container script: a `#` is literal in a double-quoted string, so a
	# quote in a "comment" there ends the string.  The named targets build the gate binaries.
	echo "=== $name ($img) ==="
	if $RT run --rm --platform "$PLATFORM" -v "$work":/src -w /src "$img" sh -ec "
		$deps
		make -j\$(nproc) PC_BUILD_REV=$REV PC_MAIL=libcurl \
			perfcached perfcli libperfd.a \
			selftest selftest_broken noisetest \
			>/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }
		./noisetest >/dev/null
		env PC_SELFTEST_LIGHT=1 ./selftest >/dev/null
		if ./selftest_broken >/dev/null 2>&1; then exit 1; fi
		./perfcached -V
		make install DESTDIR=/src/stage PREFIX=/usr PC_BUILD_REV=$REV PC_MAIL=libcurl >/dev/null
	" > "$OUT/$name.log" 2>&1; then
		# assert the ARTIFACT, not the environment: the machine type comes from
		# the ELF being shipped, not from the host's uname -m
		got=$(file -b "$work/stage/usr/bin/perfcached" 2>/dev/null)
		case "$got" in
		*"$WANT_ELF"*) ;;
		*)	echo "  FAIL $name - built the WRONG ARCHITECTURE, not shipped"
			echo "       wanted $WANT_ELF, got: $got"
			rm -rf "$work"; continue ;;
		esac

		d="$OUT/perfcached-$VER-$name-$ARCH"
		rm -rf "$d"; mkdir -p "$d"
		cp -a "$work/stage/." "$d/" 2>/dev/null
		for f in README.md ChangeLog PRODUCTION.md COPYING; do
			[ -f "$SRC/$f" ] && cp "$SRC/$f" "$d/"
		done
		tar -C "$OUT" -czf "$d.tar.gz" "$(basename "$d")"
		rm -rf "$d"
		echo "  ok   $(basename "$d.tar.gz") ($(du -h "$d.tar.gz" | cut -f1)) - $(grep -o 'perfcached .*' "$OUT/$name.log" | tail -1)"
	else
		echo "  FAIL $name - not shipped.  Last lines:"
		tail -4 "$OUT/$name.log" | sed 's/^/       /'
	fi
	rm -rf "$work"
done

echo "--- artifacts ---"
ls -1 "$OUT"/*.tar.gz 2>/dev/null | sed 's|.*/|  |' || echo "  none"
