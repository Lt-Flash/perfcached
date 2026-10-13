#!/bin/sh
# movedhinttest.sh - RV-10: a forwarded reply tells a routing client it is
# behind (see movedhinttest.c for the claim and the stale map it builds).
#
# The driver: a two-node SHARD fleet, the routing client connects and
# names its cue, a third node joins - a third of the keyspace changes
# owner under a client that will not hear about it - and the client's
# verdicts are counted.  Against a daemon without the hint the handle
# keeps two members for the whole run.
# Usage: test/movedhinttest.sh [./perfcached] [./movedhinttest]
set -u
BIN=${1:-./perfcached}
MH=${2:-./movedhinttest}
SEC=mh-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcmh.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM

mk() { # mk <node>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = mh-cluster-secret
[listen]
tcp = 127.0.66.$1:1841$1
[cluster]
multicast = 239.255.77.206:17406
advertise = 127.0.66.$1
pull_timeout_ms = 400
mode = shard
collections = c
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.log"; return 1
}
listed() { printf '%s\n' '{"method":"members"}' | timeout 30 ./perfcli -h 127.0.66.$1 -p 1841$1 -a $SEC -q 2>/dev/null | head -1 | python3 -c 'import json,sys
try: print(sum(1 for m in (json.load(sys.stdin).get("members") or []) if m.get("port")))
except Exception: print(-1)'; }

[ -x ./perfcli ] || { echo "movedhinttest: ./perfcli is missing (make perfcli) - the fleet cannot be read"; exit 1; }
for i in 1 2 3; do mk $i; done
start 1 || exit 1
start 2 || exit 1
i=0
while [ $i -lt 100 ]; do
	[ "$(listed 1)" = 2 ] && [ "$(listed 2)" = 2 ] && break
	sleep 0.2; i=$((i + 1))
done
[ "$(listed 1)" = 2 ] && ok "a two-node shard fleet formed" || { bad "the two-node fleet did not form ($(listed 1)/$(listed 2))"; exit 1; }

"$MH" 127.0.66.1 18411 "$SEC" > "$D/mh.out" 2>&1 &
MHPID=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "^ADD-NODE\$" "$D/mh.out" 2>/dev/null && break
	kill -0 $MHPID 2>/dev/null || break
	sleep 0.1; i=$((i + 1))
done
if grep -q "^ADD-NODE\$" "$D/mh.out" 2>/dev/null; then
	start 3 && ok "the third node joined on the client's cue" || bad "node 3 did not start"
else
	bad "the client never asked for the third node: $(tail -2 "$D/mh.out" | tr '\n' ' ')"
fi
wait $MHPID 2>/dev/null
sed -n 's/^ok: /  ok   /p;s/^FAIL: /  FAIL /p' "$D/mh.out"
N_OK=$(grep -c "^ok: " "$D/mh.out" 2>/dev/null); N_OK=${N_OK:-0}
N_BAD=$(grep -c "^FAIL: " "$D/mh.out" 2>/dev/null); N_BAD=${N_BAD:-0}
case "$N_OK" in ''|*[!0-9]*) N_OK=0;; esac
case "$N_BAD" in ''|*[!0-9]*) N_BAD=0;; esac
pass=$((pass + N_OK)); fail=$((fail + N_BAD))
grep -q "^movedhinttest: " "$D/mh.out" || bad "the client did not finish: $(tail -2 "$D/mh.out" | tr '\n' ' ')"

echo "movedhinttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
