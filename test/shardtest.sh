#!/bin/sh
# shardtest.sh — mode=shard: deterministic ownership (HRW over stable
# addresses).  Asserted:
#  - a fill through ONE ingress spreads across all nodes and the fleet
#    census is EXACT (each key on exactly its owner);
#  - reads through ANY node return the value (owner unicast) and store
#    nothing anywhere (census unchanged);
#  - BLIND re-writes through other nodes can never fork - deterministic
#    ownership needs no locator, no probe, no birth-race demoter;
#  - counters serialize at the owner across ingresses (exact total);
#  - misses are AUTHORITATIVE and fast: the owner answers found=0
#    explicitly, so a fleet of misses produces ZERO pull timeouts;
#  - jincr forwards to the owner (exact), mset refuses loudly;
#  - the JOIN RESHARD: a third node joins a loaded 2-node fleet, keys
#    whose HRW owner it is MOVE to it (census conserved), and reads
#    during the move never miss (the grace-window broadcast fallback).
# Usage: test/shardtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcshard.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; \
     [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport>  (advertises 127.0.0.5<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = sh-client-secret
cluster = sh-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.42:17142
advertise = 127.0.0.5$1
pull_timeout_ms = 300
[collection sh]
buckets_log2 = 12
mode = shard
EOF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
entc() { call $1 '{"method":"stats","params":{"col":"sh"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }
cstat() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }
census() { echo $(( $(entc 17021) + $(entc 17022) + $(entc 17023) )); }

# ---- phase 1: the full trio ---------------------------------------------
node 1 17021
node 2 17022
node 3 17023
start 1
start 2
start 3
sleep 3.5   # membership

# fill 300 keys through ONE ingress
for i in $(seq 0 299); do
	printf '{"method":"set","params":{"col":"sh","key":"sk%03d","value":"v%03d"}}\n' $i $i
done | python3 "$CLI" 17021 > "$D/fill.out"
NOK=$(grep -c '"stored": true' "$D/fill.out" || true)
[ "$NOK" -eq 300 ] && ok || bad "fill acked $NOK/300"
sleep 1
E1=$(entc 17021); E2=$(entc 17022); E3=$(entc 17023)
echo "shard spread: $E1 / $E2 / $E3"
[ $((E1 + E2 + E3)) -eq 300 ] && ok || bad "census $((E1+E2+E3)) != 300"
[ "$E1" -gt 20 ] && [ "$E2" -gt 20 ] && [ "$E3" -gt 20 ] && ok \
	|| bad "HRW did not spread: $E1/$E2/$E3"

# reads through every node see the value; nothing is stored anywhere
for p in 17021 17022 17023; do
	R=$(call $p '{"method":"get","params":{"col":"sh","key":"sk007"}}')
	echo "$R" | grep -q '"value": "v007"' && ok || bad "get via $p: $R"
done
[ "$(census)" -eq 300 ] && ok || bad "reads changed the census"

# BLIND re-writes through the other two nodes: ownership is
# deterministic - no locator, no probe, no fork, ever
for i in $(seq 0 299); do
	printf '{"method":"set","params":{"col":"sh","key":"sk%03d","value":"w%03d"}}\n' $i $i
done | python3 "$CLI" 17022 > /dev/null
for i in $(seq 0 299); do
	printf '{"method":"set","params":{"col":"sh","key":"sk%03d","value":"x%03d"}}\n' $i $i
done | python3 "$CLI" 17023 > /dev/null
sleep 0.5
[ "$(census)" -eq 300 ] && ok || bad "re-writes forked ($(census) != 300)"
R=$(call 17021 '{"method":"get","params":{"col":"sh","key":"sk123"}}')
echo "$R" | grep -q '"value": "x123"' && ok || bad "re-write not visible: $R"

# counters serialize at the owner across all ingresses
for p in 17021 17022 17023; do
	for i in 1 2 3 4 5; do
		printf '{"method":"add","params":{"col":"sh","key":"ctr","by":3}}\n'
	done | python3 "$CLI" $p > /dev/null
done
V=$(call 17021 '{"method":"add","params":{"col":"sh","key":"ctr","by":0}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["value"])')
[ "$V" -eq 45 ] && ok || bad "counter not serialized: $V != 45"

# authoritative misses: the owner answers found=0 explicitly - a batch
# of misses through every node must produce ZERO pull timeouts
T0=$(( $(cstat 17021 pull_timeouts) + $(cstat 17022 pull_timeouts) + $(cstat 17023 pull_timeouts) ))
for p in 17021 17022 17023; do
	for i in $(seq 0 49); do
		printf '{"method":"get","params":{"col":"sh","key":"missing%02d"}}\n' $i
	done | python3 "$CLI" $p | grep -c '"found": false' > /dev/null
done
T1=$(( $(cstat 17021 pull_timeouts) + $(cstat 17022 pull_timeouts) + $(cstat 17023 pull_timeouts) ))
[ "$T1" -eq "$T0" ] && ok || bad "misses waited for timeouts ($T0 -> $T1)"

# JSON path ops forward to the owner; mset refuses
call 17021 '{"method":"jset","params":{"col":"sh","key":"doc","path":"$","val":{"n":0}}}' > /dev/null
for p in 17021 17022 17023; do
	call $p '{"method":"jincr","params":{"col":"sh","key":"doc","path":"$.n","by":2}}' > /dev/null
done
R=$(call 17022 '{"method":"jget","params":{"col":"sh","key":"doc","path":"$.n"}}')
echo "$R" | grep -q '"value": 6' && ok || bad "jincr not serialized: $R"
R=$(call 17021 '{"method":"mset","params":{"col":"sh","items":[{"key":"a","value":"1"}]}}')
echo "$R" | grep -qi "not supported" && ok || bad "mset not refused: $R"

kill -TERM $P1 $P2 $P3 2>/dev/null; wait 2>/dev/null; P1= P2= P3=

# ---- phase 2: the join reshard ------------------------------------------
rm -f "$D"/n*.log
start 1
start 2
sleep 2.5
for i in $(seq 0 399); do
	printf '{"method":"set","params":{"col":"sh","key":"jk%03d","value":"j%03d"}}\n' $i $i
done | python3 "$CLI" 17021 > /dev/null
S0=$(( $(entc 17021) + $(entc 17022) ))
[ "$S0" -eq 400 ] && ok || bad "2-node fill census $S0 != 400"

start 3
sleep 2   # joined, reshard beginning - the GRACE window is live

# reads during the move must not miss: grace falls back to broadcast
MISS=0
for i in $(seq 0 399); do
	printf '{"method":"get","params":{"col":"sh","key":"jk%03d"}}\n' $i
done | python3 "$CLI" 17021 > "$D/during.out"
MISS=$(grep -c '"found": false' "$D/during.out" || true)
[ "$MISS" -eq 0 ] && ok || bad "$MISS reads missed during the reshard"

# convergence: node3 ends up owning its HRW share, census conserved
i=0
while [ $i -lt 12 ]; do
	E3=$(entc 17023)
	S=$(census)
	[ "$E3" -gt 40 ] && [ "$S" -eq 400 ] && break
	sleep 10; i=$((i+1))
done
E1=$(entc 17021); E2=$(entc 17022); E3=$(entc 17023)
echo "post-join spread: $E1 / $E2 / $E3"
[ $((E1 + E2 + E3)) -eq 400 ] && ok || bad "reshard lost keys ($((E1+E2+E3)) != 400)"
[ "$E3" -gt 40 ] && ok || bad "joiner received no shard ($E3)"

# after the move, reads of MOVED keys via the old holders still land
BAD=0
for i in 0 50 100 150 200 250 300 350 399; do
	R=$(call 17022 "{\"method\":\"get\",\"params\":{\"col\":\"sh\",\"key\":\"jk$(printf %03d $i)\"}}")
	echo "$R" | grep -q "\"j$(printf %03d $i)\"" || BAD=$((BAD+1))
done
[ "$BAD" -eq 0 ] && ok || bad "$BAD moved keys unreadable"

kill -TERM $P1 $P2 $P3 2>/dev/null; wait 2>/dev/null; P1= P2= P3=
echo "shardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
