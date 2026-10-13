#!/bin/sh
# libtest.sh — boots plaintext + Noise daemons and runs the libperfd C
# suite (test/libtest.c) against them.  See the .c for coverage.
# Usage: test/libtest.sh [./perfcached] [./libtest]
set -u

BIN=${1:-./perfcached}
LT=${2:-./libtest}
D=$(mktemp -d /var/tmp/pclib.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

mkconf() { # mkconf <file> <port> <plaintext>
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = lib-test-secret
cluster = lib-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = $3
[collection c]
buckets_log2 = 10
EOF
}
mkconf "$D/pt.conf" 17055 loopback
mkconf "$D/nx.conf" 17056 never
"$BIN" -f "$D/pt.conf" > "$D/pt.log" 2>&1 &
P1=$!
"$BIN" -f "$D/nx.conf" > "$D/nx.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 100 ]; do
	grep -q ready "$D/pt.log" && grep -q ready "$D/nx.log" && break
	sleep 0.1; i=$((i+1))
done

"$LT" 17055 17056 lib-test-secret
RC=$?
kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
exit $RC
