#!/bin/sh
# alpine-check.sh [--inside] - perfcached on Alpine (musl, gcc 15): both editions built with -Werror
# and check-standalone, in a podman or docker container; a host with neither SKIPS loudly.
set -u
ALPINE=3.24
IMG=localhost/pc-alpine-ci:$ALPINE
PKGS="build-base libsodium-dev libevent-dev linux-headers python3 \
py3-cryptography curl iproute2 procps git util-linux coreutils findutils \
curl-dev openssl openssl-dev pkgconf libmicrohttpd-dev"
# the image is cached by name: name it after its package list, so a change
# to PKGS builds a new one instead of reusing an image without the packages
IMG=$IMG-$(printf '%s' "$PKGS" | cksum | cut -d' ' -f1)

if [ "${1:-}" = --inside ]; then
	[ -f /etc/alpine-release ] || { echo "alpine-check --inside: not an Alpine system"; exit 1; }
	command -v gcc >/dev/null 2>&1 || apk add --no-cache $PKGS >/dev/null || exit 1
	echo "alpine-check: Alpine $(cat /etc/alpine-release), $(gcc --version | head -1), $(apk info -v 2>/dev/null | grep '^musl-[0-9]' | head -1)"
	# the copy under test is a git archive, so commit it as a repository of its own
	if ! git rev-parse --git-dir >/dev/null 2>&1; then
		git init -q && git add -A &&
		git -c user.name=alpine-check -c user.email=alpine-check@localhost \
			commit -qm "the tree under test" || exit 1
	fi
	set -e
	make clean >/dev/null 2>&1 || true
	make PC_EDITION=clustered -j"$(nproc)" perfcached
	./perfcached -V
	make clean >/dev/null
	make PC_EDITION=standalone -j"$(nproc)"
	./perfcached -V | grep -q standalone
	make PC_EDITION=standalone check-standalone
	echo "alpine-check: PASSED"
	exit 0
fi

TOP=$(cd "$(dirname "$0")/.." && pwd)
ENG=
for e in podman docker; do
	command -v $e >/dev/null 2>&1 && { ENG=$e; break; }
done
if [ -z "$ENG" ]; then
	echo "############################################################"
	echo "# alpine-check: SKIPPED - neither podman nor docker is     #"
	echo "# installed on this host.  Nothing was built on Alpine.    #"
	echo "############################################################"
	exit 0
fi
if ! $ENG image exists "$IMG" 2>/dev/null &&
   ! $ENG image inspect "$IMG" >/dev/null 2>&1; then
	echo "alpine-check: building $IMG"
	printf 'FROM docker.io/library/alpine:%s\nRUN apk add --no-cache %s\n' \
		"$ALPINE" "$PKGS" | $ENG build -q -t "$IMG" -f - "$TOP/tools" \
		>/dev/null || { echo "alpine-check: cannot build $IMG"; exit 1; }
fi
# a THROWAWAY copy of the committed tree: no host .o or config.mk inside
WORK=$(mktemp -d /var/tmp/pc-alpine.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
# mktemp makes it 0700; pintest runs the daemon as nobody (setpriv), who
# must be able to reach ./perfcached - as it can in a CI checkout
chmod 755 "$WORK"
git -C "$TOP" archive HEAD | tar -x -C "$WORK" || { echo "alpine-check: cannot export the tree"; exit 1; }
# --ulimit memlock: the container gets the caller's locked-memory limit (8 MB under systemd), which
# pintest's pin = require refuses; --shm-size: WAL suites put their segments in /dev/shm
$ENG run --rm --shm-size=1g --ulimit memlock=-1:-1 -v "$WORK:/src" -w /src "$IMG" \
	sh tools/alpine-check.sh --inside
