#!/bin/sh
# clustercfgtest.sh — the cluster owns the collection config (task S30).
#
# THE BUG THIS CLOSES, demonstrated before it was fixed: two nodes in
# one cluster, collection 'c' declared store on one and shard on the other,
# joined happily; a write through either VANISHED for the other, and
# neither log said a word.  A cluster is ONE mode over ONE collection
# set, and members that disagree must not federate.
#
# Asserted:
#  - config rules, before any traffic: [cluster] collections needs a
#    mode; a [collection X] outside the set is a parse error; a local
#    mode that contradicts the cluster's is a parse error; eager
#    params need their mode;
#  - the set travels: a collection the cluster declares but the node
#    has no block for is CREATED with default sizing, and served;
#  - matching nodes federate normally (the fix must not break agreement);
#  - MISMATCHED nodes REFUSE each other - loudly, naming both configs,
#    and neither ends up in the other's membership - for every pair:
#    store-vs-shard, store-vs-proxy, proxy-vs-shard,
#    eager-vs-not, and a differing collection SET;
#  - a restarted node with a matching config rejoins cleanly.
# Usage: test/clustercfgtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccc.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

MC=239.255.77.73
MP=17173

# conf <file> <port> <advertise-last-octet> <cluster-block> [collection-block]
conf() {
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = cc-client-secret
cluster = cc-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.6.$3
pull_timeout_ms = 300
$4
${5:-}
EOF
}
start() { # start <conf> <pidvar> <log>; 0 = came up
	"$BIN" -f "$1" > "$3" 2>&1 &
	eval "$2=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$3" && return 0
		eval "kill -0 \$$2" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stopall() {
	[ -n "$P1" ] && kill -9 $P1 2>/dev/null
	[ -n "$P2" ] && kill -9 $P2 2>/dev/null
	wait 2>/dev/null; P1=; P2=
	sleep 0.3
}
call() { printf '%s\n' "$2" | timeout 8 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=6)
r = json.loads(sys.stdin.read()); r["id"] = 1; r["jsonrpc"] = "2.0"
s.sendall(json.dumps(r).encode() + b"\n")
print(s.recv(200000).decode().strip())' "$1"; }
peers() { call $1 '{"method":"stats"}' | python3 -c \
	'import json,sys; print(json.load(sys.stdin)["result"]["cluster"]["peers_up"])' \
	2>/dev/null || echo "?"; }

# ---- 1. config rules, before any traffic ----------------------------
badconf() { # badconf <label> <cluster-block> <collection-block> <expect>
	conf "$D/bad.conf" 17301 1 "$2" "$3"
	OUT=$("$BIN" -C -f "$D/bad.conf" 2>&1)
	if [ $? -eq 0 ]; then
		bad "$1: config ACCEPTED"
	else
		echo "$OUT" | grep -qi "$4" && ok \
			|| bad "$1: wrong error: $(echo "$OUT" | tail -1)"
	fi
}
badconf "collections without mode" "collections = c" \
	"[collection c]
buckets_log2 = 10" "one cluster, one mode"
badconf "collection outside the set" "mode = store
collections = c" \
	"[collection c]
buckets_log2 = 10
[collection private]
buckets_log2 = 10" "not in the cluster"
badconf "local mode contradicts cluster" "mode = shard
collections = c" \
	"[collection c]
buckets_log2 = 10
mode = proxy" "contradicts"
badconf "ec is no longer a mode" "mode = ec
collections = c" \
	"[collection c]
buckets_log2 = 10" "store|proxy|shard"
badconf "eager without store" "mode = shard
eager = 1
collections = c" \
	"[collection c]
buckets_log2 = 10" "eager needs mode = store"

# a matching local mode is allowed (it is a hint)
conf "$D/hint.conf" 17301 1 "mode = shard
collections = c" "[collection c]
buckets_log2 = 10
mode = shard"
"$BIN" -C -f "$D/hint.conf" >/dev/null 2>&1 && ok \
	|| bad "matching local mode rejected"

# ---- 2. the set travels: no block -> created with default sizing ----
conf "$D/set.conf" 17301 1 "mode = store
collections = c, extra" "[collection c]
buckets_log2 = 10"
start "$D/set.conf" P1 "$D/set.log" || { echo "set cfg did not start"; \
	cat "$D/set.log"; exit 1; }
grep -q "collection 'extra' comes from the cluster" "$D/set.log" && ok \
	|| bad "undeclared clustered collection not created/logged"
R=$(call 17301 '{"method":"set","params":{"col":"extra","key":"k","value":"v"}}')
echo "$R" | grep -q '"stored":[ ]*true' && ok || bad "created collection unusable: $R"
stopall

# a node with NO local collection blocks at all is the supported case:
# the whole set comes from the cluster
conf "$D/bare.conf" 17301 1 "mode = store
collections = c, extra" ""
start "$D/bare.conf" P1 "$D/bare.log" || { bad "bare cluster-only config \
did not start: $(tail -2 "$D/bare.log")"; }
if [ -n "$P1" ]; then
	R=$(call 17301 '{"method":"set","params":{"col":"c","key":"b","value":"v"}}')
	echo "$R" | grep -q '"stored":[ ]*true' && ok \
		|| bad "bare config: collection unusable: $R"
	stopall
fi

# ---- 3. matching nodes federate (the fix must not break agreement) --
CL="mode = store
collections = c"
conf "$D/m1.conf" 17301 1 "$CL" "[collection c]
buckets_log2 = 10"
conf "$D/m2.conf" 17302 2 "$CL" "[collection c]
buckets_log2 = 12"       # DIFFERENT local sizing: node-local, allowed
start "$D/m1.conf" P1 "$D/m1.log" || { echo "m1 no start"; exit 1; }
start "$D/m2.conf" P2 "$D/m2.log" || { echo "m2 no start"; exit 1; }
sleep 4
A=$(peers 17301); B=$(peers 17302)
[ "$A" = "1" ] && [ "$B" = "1" ] && ok || bad "matching nodes did not federate ($A/$B)"
call 17301 '{"method":"set","params":{"col":"c","key":"shared","value":"yes"}}' >/dev/null
sleep 0.5
R=$(call 17302 '{"method":"get","params":{"col":"c","key":"shared"}}')
echo "$R" | grep -q 'yes' && ok || bad "matching cluster lost data: $R"
grep -q "REFUSING peer" "$D/m1.log" "$D/m2.log" && \
	bad "matching nodes refused each other" || ok
stopall

# ---- 4. every mismatched pair refuses, loudly and mutually ----------
pair() { # pair <label> <cluster-block-1> <cluster-block-2>
	conf "$D/p1.conf" 17301 1 "$2" "[collection c]
buckets_log2 = 10"
	conf "$D/p2.conf" 17302 2 "$3" "[collection c]
buckets_log2 = 10"
	: > "$D/p1.log"; : > "$D/p2.log"
	start "$D/p1.conf" P1 "$D/p1.log" || { bad "$1: node1 no start"; return; }
	start "$D/p2.conf" P2 "$D/p2.log" || { bad "$1: node2 no start"; return; }
	sleep 5
	A=$(peers 17301); B=$(peers 17302)
	if [ "$A" = "0" ] && [ "$B" = "0" ]; then ok
	else bad "$1: mismatched nodes federated anyway ($A/$B)"; fi
	if grep -q "REFUSING peer" "$D/p1.log" && \
	   grep -q "REFUSING peer" "$D/p2.log"; then ok
	else bad "$1: refusal not logged on both sides"; fi
	stopall
}
pair "store vs shard" "mode = store
collections = c" "mode = shard
collections = c"
pair "store vs proxy" "mode = store
collections = c" "mode = proxy
collections = c"
pair "proxy vs shard" "mode = proxy
collections = c" "mode = shard
collections = c"
pair "eager differs"  "mode = store
eager = 1
collections = c" "mode = store
collections = c"
pair "collection SET differs" "mode = store
collections = c" "mode = store
collections = c, other"

# ---- 5. a restarted node with a matching config rejoins -------------
conf "$D/r1.conf" 17301 1 "$CL" "[collection c]
buckets_log2 = 10"
conf "$D/r2.conf" 17302 2 "$CL" "[collection c]
buckets_log2 = 10"
start "$D/r1.conf" P1 "$D/r1.log" || { echo "r1 no start"; exit 1; }
start "$D/r2.conf" P2 "$D/r2.log" || { echo "r2 no start"; exit 1; }
sleep 4
[ "$(peers 17301)" = "1" ] && ok || bad "rejoin rig did not federate"
kill -9 $P2 2>/dev/null; wait $P2 2>/dev/null; P2=
sleep 7
: > "$D/r2.log"
start "$D/r2.conf" P2 "$D/r2.log" || { echo "r2 restart failed"; exit 1; }
sleep 4
A=$(peers 17301); B=$(peers 17302)
[ "$A" = "1" ] && [ "$B" = "1" ] && ok || bad "restarted node did not rejoin ($A/$B)"
grep -q "REFUSING peer" "$D/r1.log" "$D/r2.log" && \
	bad "restart triggered a spurious refusal" || ok
stopall

echo "clustercfgtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
