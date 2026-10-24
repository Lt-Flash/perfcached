#!/bin/sh
# electboundtest.sh - CH-19: a member's election defers to a live higher
# address only so long.  The highest live address self-promotes, and every
# other member waits for it - for ever, while that address beats as a
# member and never takes mastership (a replayed ALIVE from it looks exactly
# so).  The wait is now bounded by the higher node's own probe hold and a
# master's death (MPROBE_HOLD_MS + MASTER_DEAD_MS, 38 s); then it promotes.
# Three unicast nodes; node 3, the highest address, runs with
# PERFCACHED_TEST_NO_PROMOTE - it beats and follows, and never promotes.
#   1  node 1, the founder and master, is killed: node 2 defers to node 3,
#      and takes mastership within the bound - said in its log
#   2  node 3 follows it; the cluster id is unchanged
# Usage: test/electboundtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcelect.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23400
for p in $PORT 23401 23402 23451 23452 23453 23461 23462 23463; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "electboundtest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = eb-client-secret
cluster = eb-cluster-secret
[listen]
tcp = 127.0.0.1:$((23450 + $1))
http = 127.0.0.1:$((23460 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.145.1
advertise = 127.0.145.$1
mode = store
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23460 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
wait_for() { # wait_for <secs> <shell condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
role() { st "$1" 'r["cluster"]["role"]'; }

conf 1; conf 2; conf 3
# node 1 founds alone first: who founds among nodes started together is a
# race, and the suite needs node 1 the master and node 3 (never promoting,
# which does not stop a founding) a member
start 1
wait_for 30 '[ "$(role 1)" = master ]' || bad "node 1 did not found the fleet"
start 2
PERFCACHED_TEST_NO_PROMOTE=1 "$BIN" -f "$D/n3.conf" >> "$D/n3.log" 2>&1 & echo $! > "$D/n3.pid"
wait_for 40 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(role 1)" = master ]' \
	|| bad "the fleet did not form with node 1 the master: peers $(up 1)/$(up 2)/$(up 3), role $(role 1)"
grep -q "PERFCACHED_TEST_NO_PROMOTE" "$D/n3.log" || bad "node 3 does not announce its test flag"
C0=$(st 2 'r["cluster"]["cluster_id"]')

echo "--- 1: the master is killed; the highest address never promotes"
kill -9 "$(cat "$D/n1.pid")"; rm -f "$D/n1.pid"
T0=$(date +%s)
wait_for 75 '[ "$(role 2)" = master ]' \
	&& ok "node 2 took mastership $(( $(date +%s) - T0 )) s after the kill - it did not wait for node 3 for ever" \
	|| bad "no master 75 s after the kill: roles $(role 2)/$(role 3)"
grep -q "a higher address, has been live for .* without taking mastership - promoting instead" "$D/n2.log" \
	&& ok "and said why: node 3, live at a higher address, never took it" \
	|| bad "no bounded-deferral line: $(grep -h 'cluster:' "$D/n2.log" | tail -2 | cut -c1-140 | tr '\n' '|')"

echo "--- 2: node 3 follows"
wait_for 15 '[ "$(role 3)" = member ] && [ "$(st 3 "r[\"cluster\"][\"map\"][\"master\"]")" = "$(st 2 "r[\"cluster\"][\"node\"]")" ]' \
	&& ok "node 3 is a member of node 2's fleet" \
	|| bad "node 3: role $(role 3), map master $(st 3 'r["cluster"]["map"]["master"]') (node 2 is $(st 2 'r["cluster"]["node"]'))"
[ "$(st 2 'r["cluster"]["cluster_id"]')" = "$C0" ] && ok "the cluster id is unchanged" \
	|| bad "the cluster id changed: $C0 -> $(st 2 'r["cluster"]["cluster_id"]')"

echo "electboundtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
