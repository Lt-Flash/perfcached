#!/bin/sh
# release-tarballs.sh — S64 stage 1: a pre-built artifact per distro.
#
# The releases page offered only GitHub's generated source archives.
# This produces, for each distro, an FHS-correct tarball built from the
# tree's own `install` target - so the layout is exactly what a package
# would lay down later, and stage 2 inherits it rather than re-inventing
# it.
#
# Each distro builds in a THROWAWAY copy of the tree: .o files must
# never cross a libc or a compiler version.  Same rule as matrix.sh.
#
# A tarball is published ONLY if that distro's gate passes - build,
# selftest, noisetest, and the broken-lock canary (which must FAIL, or
# the gate proves nothing).  A distro that cannot pass is reported and
# skipped, never shipped quietly.  That is how musl gets an honest
# answer rather than an assumption: it either passes or it does not.
#
# Usage: tools/release-tarballs.sh [outdir] [distro ...]
set -u

SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-/var/tmp/pcrel}
shift 2>/dev/null || true
VER=$(sed -n 's/.*PC_VERSION[^"]*"\([^"]*\)".*/\1/p' "$SRC/src/version.h")
# .git is excluded from the throwaway copy, so the Makefile's
# `git rev-parse` would stamp "unknown" - a release binary that cannot
# say which revision it is defeats the point of shipping it.
REV=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)

# The platform must be EXPLICIT.  podman serves whatever variant of a
# tag is in local storage, and this host's arch matrix leaves arm/v7
# images there - so `debian:trixie-slim` quietly produced a 32-bit ARM
# binary inside a tarball named x86_64.  It passed every gate, because
# qemu-user binfmt is registered here and ran it transparently.
PLATFORM=${PLATFORM:-linux/amd64}
case "$PLATFORM" in
linux/amd64)  WANT_ELF='x86-64';        ARCH=x86_64 ;;
linux/arm64)  WANT_ELF='ARM aarch64';   ARCH=aarch64 ;;
linux/arm/v7) WANT_ELF='ARM, EABI5';    ARCH=armv7 ;;
linux/386)    WANT_ELF='Intel 80386';   ARCH=i386 ;;
*) echo "unknown PLATFORM $PLATFORM"; exit 1 ;;
esac

# image:tag  pkg-manager-line
# Rocky needs EPEL: libsodium-devel is NOT in the base repos on el9,
# which is the kind of thing that only shows up when you try it.
# Fully qualified, always: podman has no unqualified-search registries
# here, and a short name only resolves if that image happens to be in
# local storage already - which silently worked for debian/ubuntu and
# failed for rocky.
#
# alpine needs linux-headers: it keeps kernel headers in their own
# package while glibc distros ship them with libc6-dev, so
# <linux/futex.h> is absent by default.  That is a packaging
# difference, NOT musl rejecting our code.
DISTROS='
debian:docker.io/library/debian:trixie-slim:apt-get update -qq && apt-get install -y -qq build-essential libsodium-dev
ubuntu:docker.io/library/ubuntu:24.04:apt-get update -qq && apt-get install -y -qq build-essential libsodium-dev
rocky:docker.io/rockylinux/rockylinux:9:dnf install -y -q epel-release && dnf install -y -q gcc make libsodium-devel
alpine:docker.io/library/alpine:3.20:apk add --no-cache build-base linux-headers libsodium-dev
'

command -v podman >/dev/null || { echo "podman required"; exit 1; }
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
	# name:image:tag:deps, where image itself contains slashes and the
	# deps line contains colons - so peel name off the front, deps off
	# the back at the FIRST ':' that follows the tag
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

	# NOTHING but commands inside the container script.  A `#` is not a
	# comment inside a double-quoted string - it is literal text, and
	# the string keeps interpreting quotes and backticks.  A comment
	# here once contained a quoted phrase, which TERMINATED the string:
	# the container installed deps, ran nothing, exited 0, and the gate
	# passed on an empty build.  Explanations live out here instead.
	#
	# The named targets matter: the default target does not build the
	# gate binaries.  The canary must FAIL or the gate proves nothing.
	echo "=== $name ($img) ==="
	if podman run --rm --platform "$PLATFORM" -v "$work":/src -w /src "$img" sh -ec "
		$deps
		make -j\$(nproc) PC_BUILD_REV=$REV \
			perfcached perfcli libperfd.a \
			selftest selftest_broken noisetest \
			>/tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }
		./noisetest >/dev/null
		env PC_SELFTEST_LIGHT=1 ./selftest >/dev/null
		if ./selftest_broken >/dev/null 2>&1; then exit 1; fi
		./perfcached -V
		make install DESTDIR=/src/stage PREFIX=/usr PC_BUILD_REV=$REV >/dev/null
	" > "$OUT/$name.log" 2>&1; then
		# Assert the ARTIFACT, not the environment: read the machine
		# type out of the ELF we are about to ship.  The label used to
		# come from uname -m on the host, which is how an ARM binary
		# ended up in a tarball named x86_64.
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
		for f in README.md CHANGELOG.md PRODUCTION.md COPYING; do
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
