#!/bin/sh
# phptest.sh — Perfcached.php (the pure-PHP client) against live
# daemons: plaintext + the Noise channel with secret rotation.  The PHP
# side reimplements the handshake from the spec - passing here proves a
# THIRD independent implementation (C, python, PHP) interoperates.
# SKIPS VISIBLY when php is absent (the build host carries it).
# Usage: test/phptest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
HERE=$(dirname "$0")
if ! command -v php >/dev/null 2>&1; then
	echo "phptest: SKIPPED - php not installed on this host"
	exit 0
fi
D=$(mktemp -d /var/tmp/pcphp.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

mkconf() {
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = php-test-secret
cluster = php-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = $3
[collection c]
buckets_log2 = 10
EOF
}
mkconf "$D/pt.conf" 17057 loopback
mkconf "$D/nx.conf" 17058 never
"$BIN" -f "$D/pt.conf" > "$D/pt.log" 2>&1 &
P1=$!
"$BIN" -f "$D/nx.conf" > "$D/nx.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 100 ]; do
	grep -q ready "$D/pt.log" && grep -q ready "$D/nx.log" && break
	sleep 0.1; i=$((i+1))
done

php "$HERE/phptest.php" 17057 17058 php-test-secret
RC=$?
kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
exit $RC
