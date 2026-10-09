#!/bin/sh
# alpine-check.sh - S285: perfcached on Alpine - musl and gcc 15 - built
# and tested the way the Debian jobs are.
#
# Alpine is a supported runtime from 0.5.0 (DESIGN 12hd).  What this
# gates, in a throwaway copy of the tree inside an alpine container:
#   - both editions compile with the Makefile's own -Werror (gcc 15 is
#     where the flen warning of 6bd124e came from - S272)
#   - the standalone edition passes make check-standalone
# The clustered suites need multicast and network namespaces, which a
# plain container does not give them; the clustered edition is compiled
# here, and its suites run on the Debian jobs.
#
# Two modes, one script:
#   tools/alpine-check.sh           on the host: build (or reuse) the
#                                   toolchain image and run the check in
#                                   it, with podman or docker
#   tools/alpine-check.sh --inside  inside an Alpine container: the check
# A host with neither podman nor docker SKIPS loudly (the matrix.sh
# contract): never a failure that looks like a code regression, never a
# pass that looks like coverage.
# /dev/shm is 1 GB in the container: the WAL suites put their segments
# there (walprobetest, healtest, rdbkilltest), and a container's default
# is 64 MB (S271).
set -u
ALPINE=3.24
IMG=localhost/pc-alpine-ci:$ALPINE
PKGS="build-base libsodium-dev libevent-dev linux-headers python3 \
py3-cryptography curl iproute2 procps git util-linux coreutils findutils"

if [ "${1:-}" = --inside ]; then
	[ -f /etc/alpine-release ] || { echo "alpine-check --inside: not an Alpine system"; exit 1; }
	command -v gcc >/dev/null 2>&1 || apk add --no-cache $PKGS >/dev/null || exit 1
	echo "alpine-check: Alpine $(cat /etc/alpine-release), $(gcc --version | head -1), $(apk info -v 2>/dev/null | grep '^musl-[0-9]' | head -1)"
	# synctest exports libperfd's MIT file set with git; the copy under
	# test is a git archive, so commit it as a repository of its own
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
# mktemp makes it 0700; pintest runs the daemon as nobody (setpriv), who
# must be able to reach ./perfcached - as it can in a CI checkout
chmod 755 "$WORK"
git -C "$TOP" archive HEAD | tar -x -C "$WORK" || { echo "alpine-check: cannot export the tree"; exit 1; }
# --ulimit memlock: a container's root has no CAP_IPC_LOCK, so the locked-
# memory limit is the one the CONTAINER gets - by default the caller's.
# Under a systemd service (the GitLab runner; any unit) that is 8 MB, and
# pintest's pin = require leg then refuses its 32 MB arena exactly as the
# daemon is designed to: red for the caller's limit, not the code.  Seen
# 2026-10-05 (DESIGN 12iq).  Unlimited here, as an ssh shell on the
# runner hosts effectively gives - where the engine can: a rootless one
# cannot raise it past its caller's hard limit (GitHub's runner, 8 MB),
# and pintest then skips its locking half and says so.
$ENG run --rm --shm-size=1g --ulimit memlock=-1:-1 -v "$WORK:/src" -w /src "$IMG" \
	sh tools/alpine-check.sh --inside
