#!/bin/sh
# matrix.sh — the S27 four-arch build+test matrix (Linux-only, by
# decree: x86_64, i386, arm64, arm32).
#
# Runs on the build host (224) with podman + qemu-user binfmt.  Each
# arch gets a debian:trixie-slim toolchain image (built once, reused)
# and a THROWAWAY copy of the tree - .o files never cross arches.
#
# What runs where:
#  - all four:  make perfcached selftest selftest_broken noisetest
#               memprobe;  ./noisetest;  ./selftest;  and the canary -
#               selftest_broken MUST FAIL (a lock shim that cannot fail
#               detectably proves nothing).
#  - amd64+i386 (native execution): full-scale selftest + memprobe.
#  - arm64+arm32 (qemu-user): PC_SELFTEST_LIGHT scale - emulation is
#    8-16x slower and the storm would trip the livelock alarm - and NO
#    memprobe: qemu-user's guest addresses do not line up with
#    /proc/self/smaps, so tier detection would lie.  Documented, not
#    papered over.
#
# ~2-6 min/arch under emulation.  Not part of make check; run it before
# a release or after touching compat/, core/ or anything word-size- or
# endian-sensitive.
# Usage: tools/matrix.sh [arch...]   (default: all four)
set -u

ARCHES=${*:-amd64 i386 arm64 arm32}
BASE=docker.io/library/debian:trixie-slim
TOP=$(cd "$(dirname "$0")/.." && pwd)
WORK=/var/tmp/pc-matrix          # NOT /tmp - tmpfs on the build host
failed=""

platform_of() {
	case $1 in
	amd64) echo linux/amd64 ;;
	i386)  echo linux/386 ;;
	arm64) echo linux/arm64 ;;
	arm32) echo linux/arm/v7 ;;
	*)     echo "unknown arch $1" >&2; exit 2 ;;
	esac
}

image_of() { echo "localhost/pc-build-$1"; }

build_image() { # build_image <arch>
	podman image exists "$(image_of "$1")" && return 0
	echo "== $1: building toolchain image"
	podman build --platform "$(platform_of "$1")" -t "$(image_of "$1")" \
		-f - "$TOP" <<EOF
FROM $BASE
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make libc6-dev libsodium-dev && rm -rf /var/lib/apt/lists/*
EOF
}

run_arch() { # run_arch <arch>
	a=$1
	d=$WORK/$a
	light=""
	probe="./memprobe"
	case $a in arm64|arm32)
		light="PC_SELFTEST_LIGHT=1"
		probe="echo 'memprobe: SKIPPED under qemu-user (smaps/guest-address mismatch)'"
	esac
	rm -rf "$d"
	mkdir -p "$d"
	(cd "$TOP" && tar cf - --exclude .git --exclude '*.o' \
		--exclude perfcached --exclude selftest \
		--exclude selftest_broken --exclude noisetest \
		--exclude memprobe .) | (cd "$d" && tar xf -)
	echo "== $a: build + test ($(platform_of "$a"))"
	podman run --rm --platform "$(platform_of "$a")" \
		-v "$d":/w -w /w \
		"$(image_of "$a")" sh -ec "
		# uname lies under --platform (the host kernel answers); the
		# userland word size is the truth that matters here
		echo \"arch: \$(uname -m), userland LONG_BIT=\$(getconf LONG_BIT)\"
		make perfcached selftest selftest_broken noisetest memprobe \
			> build.log 2>&1 || { tail -30 build.log; exit 1; }
		./noisetest
		env $light ./selftest
		if ./selftest_broken > /dev/null 2>&1; then
			echo 'selftest_broken PASSED - the canary is dead'
			exit 1
		fi
		echo 'selftest_broken failed as it must'
		$probe
		./perfcached -V
	"
}

for a in $ARCHES; do
	build_image "$a" || { failed="$failed $a(image)"; continue; }
	if run_arch "$a"; then
		echo "== $a: PASS"
	else
		echo "== $a: FAIL"
		failed="$failed $a"
	fi
done

echo
if [ -n "$failed" ]; then
	echo "matrix: FAILED on:$failed"
	exit 1
fi
echo "matrix: all arches green ($ARCHES)"
