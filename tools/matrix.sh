#!/bin/sh
# matrix.sh - the four-arch build+test matrix (x86_64, i386 native; arm64, arm32 under qemu-user at light
# scale, no memprobe); run before a release or after touching compat/, core/ or word-size/endian code.
set -u

ARCHES=${*:-amd64 i386 arm64 arm32}
BASE=docker.io/library/debian:trixie-slim
TOP=$(cd "$(dirname "$0")/.." && pwd)
WORK=/var/tmp/pc-matrix          # NOT /tmp - tmpfs on the build host
failed=""

# ---- preflight ----------------------------------------------------------
# a runner without podman or qemu-user binfmt skips LOUDLY: neither a fake failure nor fake coverage
if ! command -v podman >/dev/null 2>&1; then
	echo "############################################################"
	echo "# matrix: SKIPPED - podman is not installed on this host.  #"
	echo "# No architecture was built or tested by this run.         #"
	echo "############################################################"
	exit 0
fi

# qemu-user binfmt is what makes the arm arches runnable at all; without it
# they would fail with 'exec format error' and look like real failures.
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] &&
   [ ! -e /proc/sys/fs/binfmt_misc/qemu-arm ]; then
	NATIVE=""
	for a in $ARCHES; do
		case $a in amd64|i386) NATIVE="$NATIVE $a" ;; esac
	done
	echo "############################################################"
	echo "# matrix: qemu-user binfmt is NOT registered on this host. #"
	echo "# The emulated arches are SKIPPED, not tested:             #"
	echo "#   asked for:$ARCHES"
	echo "#   running:  ${NATIVE:- (none)}"
	echo "############################################################"
	[ -n "$NATIVE" ] || exit 0
	ARCHES=$NATIVE
fi

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
	ubsan="echo 'ubsan: not run on this arch'"
	case $a in arm64|arm32)
		light="PC_SELFTEST_LIGHT=1"
		probe="echo 'memprobe: SKIPPED under qemu-user (smaps/guest-address mismatch)'"
	esac
	# UBSan on arm64 only: shift-width UB is what a different architecture surfaces (x86's shift-wrap
	# hid one); ASan is absent, as user-mode emulation reproduces its shadow memory badly
	case $a in arm64)
		ubsan="echo '--- rebuilding selftest+noisetest under UBSan ---'
		make SAN='-fsanitize=undefined -fno-omit-frame-pointer' \
			selftest noisetest > ubsan-build.log 2>&1 ||
			{ tail -25 ubsan-build.log; exit 1; }
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./noisetest
		env $light UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			./selftest
		echo 'ubsan: clean on aarch64'"
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
		$ubsan
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
