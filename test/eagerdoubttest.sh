#!/bin/sh
# eagerdoubttest.sh - S464: the doubt follows its source.
#
# Three eager nodes, past their own 30 s start windows (S305 keeps those).
#  1  node 3 is killed -9 and comes back COLD (its WAL wiped): it
#     bootstraps from the fleet and holds nothing new, so node 1 doubts
#     nobody - its misses stay final and no pull goes out, none is served;
#  2  node 3 authors keys, is killed -9 and comes back RECOVERED (its WAL
#     replayed): node 1 doubts node 3 alone - a miss asks node 3, and node
#     2 serves none of them.
# Both restarts fall inside PEER_UP_MS (10 s), so the peer never looked gone.
# Fail-first: before S464 every peer's return reopened 30 s of every miss
# broadcast to every peer - arm 1 sends pulls and both peers serve them,
# arm 2 has node 2 serving; S464 as first built missed arm 2's return.
# Usage: test/eagerdoubttest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
SEC=ed-client-secret
D=$(mktemp -d /var/tmp/pced.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "eagerdoubttest: $CLI is missing (make perfcli)"; exit 1; }

# node <n>: 127.0.78.<n>:2477<n>
conf() {
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ed-cluster-secret
[listen]
tcp = 127.0.78.$1:2477$1
[cluster]
multicast = 239.255.81.13:24770
advertise = 127.0.78.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 10
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval "P$1=$!"; PIDS="$PIDS $!"
}
ready() { # ready <n> <secs>
	i=0
	while [ $i -lt $(($2 * 10)) ]; do
		[ "$(st $1 'r.get("state")')" = ready ] && return 0
		sleep 0.1; i=$((i + 1))
	done
	return 1
}
st() { # st <n> <python expr over r = /stats result>
	printf '{"method":"stats"}\n' | "$CLI" -h 127.0.78.$1 -p 2477$1 -a "$SEC" -q 2>/dev/null |
		python3 -c "import json,sys
try: r = json.loads(sys.stdin.readline())
except Exception: r = {}
r = r.get('result', r)
c = r.get('cluster') or {}
print($2)" 2>/dev/null
}
ctr() { st $1 "c.get('$2', -1)"; }
batch() { # batch <n> <method> <prefix> <count>
	python3 -c "import json
for i in range($4): print(json.dumps({'method': '$2', 'params': {'col': 'c', 'key': '$3%d' % i, 'value': 'v%d' % i}}))" |
		"$CLI" -h 127.0.78.$1 -p 2477$1 -a "$SEC" -q 2>/dev/null | grep -c .
}
# misses <arm>: 200 absent keys read on node 1; the pull counters around them
misses() {
	S1=$(ctr 1 pull_sent); V2=$(ctr 2 pull_served); V3=$(ctr 3 pull_served)
	FINAL=$(st 1 "c.get('eager_miss_final')")
	batch 1 get "$1-absent" 200 > /dev/null
	S1=$(( $(ctr 1 pull_sent) - S1 )); V2=$(( $(ctr 2 pull_served) - V2 )); V3=$(( $(ctr 3 pull_served) - V3 ))
}

for n in 1 2 3; do conf $n; done
start 1; sleep 1; start 2; start 3
for n in 1 2 3; do ready $n 30 || { bad "node $n did not reach ready"; echo "eagerdoubttest: $pass passed, $fail failed"; exit 1; }; done
batch 1 set base 100 > /dev/null
sleep 32                                   # past every node's own start window
[ "$(st 1 "c.get('eager_miss_final')")" = True ] && ok "three eager nodes, 32 s on: node 1's misses are final (the control)" \
	|| bad "node 1's misses are not final 32 s on: $(st 1 "(c.get('eager_miss_final'), r.get('state'))")"

echo "--- 1: node 3 back cold"
kill -9 "$P3"; sleep 1
rm -rf "$D/s3/wal"; mkdir -p "$D/s3/wal"
start 3
ready 3 30 || bad "node 3 did not come back ready"
sleep 2
misses cold
[ "$FINAL" = True ] && [ "$S1" = 0 ] && [ "$V2" = 0 ] && [ "$V3" = 0 ] \
	&& ok "node 3 back cold (bootstrapped): node 1 doubts nobody - its misses stay final, 0 pulls sent, 0 served by nodes 2 and 3" \
	|| bad "after a cold return: node 1 final $FINAL, pulls sent $S1, served by node 2 $V2, by node 3 $V3"
grep -q "node [0-9]* is back" "$D/n1.log" && bad "node 1 logged a doubt for a cold return: $(grep -m1 'is back' "$D/n1.log" | cut -c1-120)" \
	|| ok "and node 1's log names no doubt"

echo "--- 2: node 3 back with a recovered log"
[ "$(batch 3 set own 50)" = 50 ] || bad "node 3 did not take its own keys"
sleep 2
kill -9 "$P3"; sleep 1
start 3
ready 3 30 || bad "node 3 did not come back ready"
sleep 2
misses rec
[ "$FINAL" = False ] && [ "$S1" -ge 1 ] && [ "$V3" -ge 1 ] && [ "$V2" = 0 ] \
	&& ok "node 3 back RECOVERED: node 1 doubts it alone - $S1 pull(s) sent, node 3 served $V3, node 2 served 0" \
	|| bad "after a recovered return: node 1 final $FINAL, pulls sent $S1, served by node 2 $V2, by node 3 $V3"
grep -q "is back with a recovered log" "$D/n1.log" \
	&& ok "node 1's log says why: $(grep -m1 'is back with a recovered log' "$D/n1.log" | sed 's/.*cluster: //' | cut -c1-70)" \
	|| bad "node 1's log names no doubt: $(grep -m1 'is back' "$D/n1.log" | cut -c1-100)"

echo "eagerdoubttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
