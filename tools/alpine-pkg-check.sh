#!/bin/sh
# alpine-pkg-check.sh - PD-11: the Alpine packages, built and proven the way
# alpine-check.sh proves the build - inside an alpine container, with
# podman or docker; a host with neither SKIPS loudly.
#
# What it gates, from the COMMITTED tree (git archive HEAD):
#   - abuild builds the six packages from contrib/alpine/APKBUILD
#   - apk installs perfcached, libperfd, perfcached-dashboard, perfcached-doc:
#     the binaries answer -V, INSTALL-MODE says package, both accounts exist
#   - rc-service perfcached start runs the daemon as perfcached and it says
#     ready; stop stops it (OpenRC needs its runlevel initialised once in a
#     bare container: rc_sys=docker, a softlevel, `openrc`)
#   - apk del perfcached + apk add perfcached-standalone: the standalone
#     edition under the same service, ready
#   - rc-service perfcached-dashboard start: its own account, /health 503
#     while no member is up (the designed answer)
# The container cannot raise rlimits or make cgroups: those warnings of
# OpenRC are filtered, not failures.
#
#   tools/alpine-pkg-check.sh             on the host
#   tools/alpine-pkg-check.sh --build     inside: abuild
#   tools/alpine-pkg-check.sh --install   inside: install, start, swap
set -u
ALPINE=${ALPINE:-3.21}
VER=$(sed -n 's/.*PC_VERSION[^"]*"\([^"]*\)".*/\1/p' "$(dirname "$0")/../src/version.h" 2>/dev/null || true)

fail() { echo "alpine-pkg-check: FAILED - $*"; exit 1; }

if [ "${1:-}" = --build ]; then
	apk update > /dev/null 2>&1 && apk add alpine-sdk sudo > /dev/null 2>&1 || fail "apk add alpine-sdk"
	adduser -D builder && addgroup builder abuild && echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder
	cp -r /work/aports /home/builder/aports && chown -R builder /home/builder
	su builder -c 'abuild-keygen -a -i -n -q > /dev/null 2>&1; cd ~/aports/perfcached && abuild checksum > /dev/null 2>&1 && abuild -r' > /work/abuild.log 2>&1 || {
		grep -E '^>>> ERROR|error:' /work/abuild.log | head -20; fail "abuild (see abuild.log)"; }
	P=$(ls -d /home/builder/packages/*/x86_64 2>/dev/null | head -1)
	mkdir -p /work/pkgs && cp "$P"/*.apk /work/pkgs/
	n=$(ls /work/pkgs/*.apk | wc -l); [ "$n" = 6 ] || fail "$n packages, not 6"
	echo "  ok   abuild: 6 packages ($(ls /work/pkgs | sed 's/-[0-9].*//' | tr '\n' ' '))"
	exit 0
fi

if [ "${1:-}" = --install ]; then
	V=$2; P=/work/pkgs
	apk update > /dev/null 2>&1 && apk add openrc > /dev/null 2>&1 || fail "apk add openrc"
	apk add --allow-untrusted $P/perfcached-$V-r0.apk $P/libperfd-$V-r0.apk $P/perfcached-dashboard-$V-r0.apk $P/perfcached-doc-$V-r0.apk > /dev/null 2>&1 || fail "apk add"
	perfcached -V | head -1 | grep -q "perfcached $V" || fail "perfcached -V: $(perfcached -V | head -1)"
	perfcached-dashboard -V | grep -q "perfcached-dashboard $V" || fail "dashboard -V"
	[ "$(cat /usr/share/perfcached/INSTALL-MODE)" = package ] || fail "INSTALL-MODE"
	id perfcached > /dev/null 2>&1 && id perfcached-dashboard > /dev/null 2>&1 || fail "the accounts"
	[ -x /etc/init.d/perfcached ] && [ -x /etc/init.d/perfcached-dashboard ] || fail "the services"
	[ -s /usr/share/doc/perfcached/GUIDE.md ] || fail "the doc package"
	echo "  ok   installed: -V, INSTALL-MODE package, accounts, services, doc"
	sed -e 's/^client = .*/client = check-client-secret/' -e 's/^cluster = .*/cluster = check-cluster-secret/' \
		/etc/perfcached/perfcached.conf.example > /etc/perfcached/perfcached.conf
	chmod 640 /etc/perfcached/perfcached.conf && chown root:perfcached /etc/perfcached/perfcached.conf
	grep -q '^rc_sys=' /etc/rc.conf && sed -i 's/^rc_sys=.*/rc_sys="docker"/' /etc/rc.conf || echo 'rc_sys="docker"' >> /etc/rc.conf
	mkdir -p /run/openrc && touch /run/openrc/softlevel && timeout 60 openrc > /dev/null 2>&1
	q() { "$@" 2>&1 | grep -v 'error setting limit\|RC_ULIMIT\|cgroup/openrc\|correcting owner\|creating '; }
	q rc-service perfcached start > /dev/null || fail "rc-service perfcached start"
	i=0; while [ $i -lt 30 ] && ! grep -q 'perfcached ready' /var/log/perfcached.log 2>/dev/null; do sleep 1; i=$((i+1)); done
	grep -q 'perfcached ready' /var/log/perfcached.log || { tail -5 /var/log/perfcached.log; fail "the daemon did not say ready"; }
	ps -o user,comm | grep -q '^perfcach[a-z]* *perfcached$' || fail "not running as perfcached: $(ps -o user,comm | grep perfcached)"
	echo "  ok   rc-service perfcached start: ready as perfcached after $i s"
	q rc-service perfcached stop > /dev/null || fail "stop"
	sleep 1; ps -o comm | grep -qx perfcached && fail "still running after stop"
	echo "  ok   stop"
	apk del perfcached > /dev/null 2>&1 && apk add --allow-untrusted $P/perfcached-standalone-$V-r0.apk > /dev/null 2>&1 || fail "the standalone swap"
	perfcached -V | head -1 | grep -q ' standalone$' || fail "not the standalone edition: $(perfcached -V | head -1)"
	q rc-service perfcached start > /dev/null || fail "standalone start"
	i=0; while [ $i -lt 30 ] && [ "$(grep -c 'perfcached ready' /var/log/perfcached.log)" -lt 2 ]; do sleep 1; i=$((i+1)); done
	[ "$(grep -c 'perfcached ready' /var/log/perfcached.log)" -ge 2 ] || fail "the standalone daemon did not say ready"
	q rc-service perfcached stop > /dev/null
	echo "  ok   perfcached-standalone replaces it under the same service: ready"
	printf '[dashboard]\nlisten = 127.0.0.1:8090\nseed = 127.0.0.1:6479\nsecret = check-client-secret\n' > /etc/perfcached/perfcached-dashboard.conf
	chmod 640 /etc/perfcached/perfcached-dashboard.conf && chown root:perfcached-dashboard /etc/perfcached/perfcached-dashboard.conf
	q rc-service perfcached-dashboard start > /dev/null || fail "dashboard start"
	sleep 1
	# busybox ps: the user to 8 characters, the command to 15
	ps -o user,comm | grep -q '^perfcach[a-z-]* *perfcached-dash' || fail "the dashboard is not running as its user: $(ps -o user,comm | grep dash)"
	h=$(wget -q -S -O /dev/null http://127.0.0.1:8090/health 2>&1 | grep 'HTTP/' | head -1)
	echo "$h" | grep -q ' 503 ' || fail "/health with no member up: '$h' (503 expected)"
	q rc-service perfcached-dashboard stop > /dev/null
	echo "  ok   perfcached-dashboard: its own account, /health 503 with no member up"
	echo "alpine-pkg-check: PASSED ($(cat /etc/alpine-release))"
	exit 0
fi

E=
for e in podman docker; do command -v $e > /dev/null 2>&1 && { E=$e; break; }; done
if [ -z "$E" ]; then
	echo "################################################################"
	echo "# alpine-pkg-check: SKIPPED - neither podman nor docker is here #"
	echo "################################################################"
	exit 0
fi
SRC=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$VER" ] || fail "no PC_VERSION in src/version.h"
W=$(mktemp -d /var/tmp/pcapk.XXXXXX)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/aports/perfcached"
git -C "$SRC" archive --format=tar.gz --prefix="perfcached-$VER/" -o "$W/aports/perfcached/perfcached-$VER.tar.gz" HEAD || fail "git archive"
cp "$SRC"/contrib/alpine/APKBUILD "$SRC"/contrib/alpine/*.initd "$SRC"/contrib/alpine/*.pre-install "$W/aports/perfcached/"
cp "$SRC"/tools/alpine-pkg-check.sh "$W/check.sh"
echo "=== alpine-pkg-check: perfcached $VER in alpine:$ALPINE with $E"
$E run --rm -v "$W:/work:z" docker.io/library/alpine:$ALPINE sh /work/check.sh --build || exit 1
$E run --rm -v "$W:/work:z" docker.io/library/alpine:$ALPINE sh /work/check.sh --install "$VER" || exit 1
