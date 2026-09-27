#!/bin/sh
# expiretest.sh - S213: a TTL re-arm is a write and takes the write's
# road in every mode.
#
# EXPIRE used to touch whatever the local table held.  A passive copy
# was re-armed and left passive - the author's clock ran out while this
# node's copy lived on - which is the RGS re-registration path: EXISTS
# says yes, only EXPIRE follows, and a balancer lands it on a node at
# random.  Measured on the fleet 2026-09-21: a device record on ONE node
# with a fresh TTL and gone from the other two, and the JSON.DEL of its
# de-registration landing on a node without it deleted nothing.  And a
# key held by another node answered "absent": expire was the one
# mutating verb without a mode branch, while set, del and add/sub all
# forward to the owner.
#
# Now a re-arm on a copy ADOPTS it (the record becomes this node's, in
# the WAL whole, pushed as authored), and a re-arm for a key held
# elsewhere forwards: shard to the owner, proxy to the locator's holder
# or by the probe set/add use, spread to the best-ranked holder.
#
# Arms, each its own fleet, each asserted on EVERY node:
#   eager  - a re-arm via the copy-only node moves the author's clock and
#            the third node's; the RGS sequence on the RESP dialect
#            (JSON.SET+EXPIRE on A, EXPIRE on B once A's short clock has
#            run out, JSON.DEL on C) leaves nothing on any node;
#   shard  - a re-arm via a non-owner answers true and moves the owner;
#   proxy  - a re-arm via a node that never saw the key (the probe) and
#            via one that read it (the locator) both move the holder;
#   spread - a re-arm via every node answers true, the K copies move and
#            no extra copy appears;
#   store  - a re-arm on a kept pull survives the re-arming node's
#            restart (it is in that node's WAL now).
# Before the fix every S213 line is red; the controls stay green.
# Usage: test/expiretest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcexp.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode() + b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
RESP="$D/resp.py"
cat > "$RESP" <<'EOF'
# one RESP command per stdin line, words split on spaces; the reply's first line
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
f = s.makefile("rwb")
for line in sys.stdin:
    a = line.strip().split(" ")
    if not a or not a[0]: continue
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()); f.flush()
    r = f.readline().decode("latin1").strip()
    if r.startswith("$") and not r.startswith("$-1"):
        f.read(int(r[1:]) + 2)
    print(r)
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
resp() { echo "$2" | python3 "$RESP" "$1"; }
jv() { python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get(\"$1\", d) if isinstance(d, dict) else d)" 2>/dev/null; }
ttl() { call "$1" "{\"method\":\"ttl\",\"params\":{\"col\":\"0\",\"key\":\"$2\"}}" | jv ttl; }
gttl() { call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"0\",\"key\":\"$2\"}}" | jv ttl; }
expire() { call "$1" "{\"method\":\"expire\",\"params\":{\"col\":\"0\",\"key\":\"$2\",\"ttl\":$3}}" | jv updated; }
entries() { call "$1" '{"method":"stats","params":{"col":"0"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }

# node <arm> <n> <cluster-lines> <collection-lines> [wal]
# arm A gets ports 181A1.., advertises 127.0.58.<A><n>, multicast group .16<A>
node() {
	mkdir -p "$D/st$1$2" "$D/wal$1$2"
	WAL=""
	[ "${5:-}" = wal ] && WAL="[wal]
dir = $D/wal$1$2
segments = 2
segment_mb = 8
fsync = always"
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/st$1$2
[memory]
arena_mb = 64
[secrets]
client = ex-client-secret
cluster = ex-cluster-secret
[listen]
tcp = 127.0.0.1:181$1$2
plaintext = loopback
[cluster]
multicast = 239.255.77.16$1:1736$1
advertise = 127.0.58.$1$2
pull_timeout_ms = 300
$3
collections = 0
[collection 0]
buckets_log2 = 10
pull = 1
$4
$WAL
EOF
}
start() { # start <arm> <n>
	"$BIN" -f "$D/n$1$2.conf" > "$D/n$1$2.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "node state .* -> ready" "$D/n$1$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1$2 never reported ready:"; tail -3 "$D/n$1$2.log"; exit 1
}
stop_arm() { # stop_arm <arm>: kill this arm's daemons, forget them
	for p in $PIDS; do
		grep -qa -- "$D/n$1" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"
	done
	PIDS=""
	sleep 0.5
}

# ---- eager ---------------------------------------------------------
for n in 1 2 3; do node 1 $n "mode = eager" ""; done
for n in 1 2 3; do start 1 $n; done
sleep 1
call 18111 '{"method":"set","params":{"col":"0","key":"k1","value":"v","ttl":60}}' > /dev/null
sleep 1
R=$(expire 18112 k1 500)
sleep 1
T1=$(ttl 18111 k1); T2=$(ttl 18112 k1); T3=$(ttl 18113 k1)
[ "$R" = True ] && [ "${T1:-0}" -gt 400 ] && [ "${T2:-0}" -gt 400 ] && [ "${T3:-0}" -gt 400 ] \
	&& ok "eager: a re-arm on the copy-only node moved the author's clock and the third node's (ttl $T1/$T2/$T3)" \
	|| bad "eager: a re-arm on the copy-only node stayed there (updated=$R ttl author=$T1 copy=$T2 third=$T3)"
# the RGS sequence, on the RESP dialect through three different nodes
resp 18111 'JSON.SET dev $ {"status":"Reachable"}' > /dev/null
resp 18111 'EXPIRE dev 2' > /dev/null
sleep 0.5
R=$(resp 18112 'EXPIRE dev 500')
sleep 3                                  # the short clock runs out on A and C
E1=$(resp 18111 'EXISTS dev'); E3=$(resp 18113 'EXISTS dev')
[ "$R" = ":1" ] && [ "$E1" = ":1" ] && [ "$E3" = ":1" ] \
	&& ok "eager/RESP: the re-registration's EXPIRE via B kept the record alive on A and C" \
	|| bad "eager/RESP: after EXPIRE via B ($R) the record is gone from A ($E1) or C ($E3)"
R=$(resp 18113 'JSON.DEL dev')
sleep 1
E1=$(resp 18111 'EXISTS dev'); E2=$(resp 18112 'EXISTS dev'); E3=$(resp 18113 'EXISTS dev')
[ "$R" = ":1" ] && [ "$E1" = ":0" ] && [ "$E2" = ":0" ] && [ "$E3" = ":0" ] \
	&& ok "eager/RESP: the de-registration's JSON.DEL via C deleted it, and it is gone from every node" \
	|| bad "eager/RESP: JSON.DEL via C answered $R and the record remains (A=$E1 B=$E2 C=$E3) - the ghost registration"
# control: a re-arm on the AUTHOR still travels
call 18111 '{"method":"set","params":{"col":"0","key":"k1c","value":"v","ttl":60}}' > /dev/null
sleep 1
R=$(expire 18111 k1c 500); sleep 1; T2=$(ttl 18112 k1c)
[ "$R" = True ] && [ "${T2:-0}" -gt 400 ] && ok "eager control: a re-arm on the author travels (ttl on B $T2)" \
	|| bad "eager control: a re-arm on the author did not travel (updated=$R ttl on B $T2)"
stop_arm 1

# ---- shard ---------------------------------------------------------
for n in 1 2 3; do node 2 $n "mode = shard" ""; done
for n in 1 2 3; do start 2 $n; done
sleep 1
call 18121 '{"method":"set","params":{"col":"0","key":"k2","value":"v","ttl":60}}' > /dev/null
sleep 0.5
R2=$(expire 18122 k2 500); R3=$(expire 18123 k2 500)
sleep 0.5
G1=$(gttl 18121 k2); G2=$(gttl 18122 k2); G3=$(gttl 18123 k2)
[ "$R2" = True ] && [ "$R3" = True ] && [ "${G1:-0}" -gt 400 ] && [ "${G2:-0}" -gt 400 ] && [ "${G3:-0}" -gt 400 ] \
	&& ok "shard: a re-arm via every ingress answers true and the owner's record moved (ttl $G1/$G2/$G3)" \
	|| bad "shard: re-arm via non-owners (updated $R2/$R3) did not reach the owner (ttl $G1/$G2/$G3)"
R=$(expire 18122 nosuch 500)
[ "$R" = False ] && ok "shard: a re-arm of a key nobody has answers false" || bad "shard: a re-arm of a missing key answered $R"
stop_arm 2

# ---- proxy ---------------------------------------------------------
for n in 1 2 3; do node 3 $n "mode = proxy" ""; done
for n in 1 2 3; do start 3 $n; done
sleep 1
call 18131 '{"method":"set","params":{"col":"0","key":"k3","value":"v","ttl":60}}' > /dev/null
sleep 0.5
R3=$(expire 18133 k3 500)                 # node C never saw the key: the probe road
call 18132 '{"method":"get","params":{"col":"0","key":"k3"}}' > /dev/null   # B learns the holder
R2=$(expire 18132 k3 600)                 # the locator road
sleep 0.5
G1=$(gttl 18131 k3); G2=$(gttl 18132 k3); G3=$(gttl 18133 k3)
[ "$R3" = True ] && [ "$R2" = True ] && [ "${G1:-0}" -gt 500 ] && [ "${G2:-0}" -gt 500 ] && [ "${G3:-0}" -gt 500 ] \
	&& ok "proxy: re-arms via the probe and via the locator both moved the holder (ttl $G1/$G2/$G3)" \
	|| bad "proxy: re-arm via probe=$R3 locator=$R2, holder ttl $G1/$G2/$G3"
R=$(expire 18133 nosuch 500)
[ "$R" = False ] && ok "proxy: a re-arm of a key nobody has answers false" || bad "proxy: a re-arm of a missing key answered $R"
stop_arm 3

# ---- spread --------------------------------------------------------
for n in 1 2 3; do node 4 $n "mode = spread
replicas = 2" ""; done
for n in 1 2 3; do start 4 $n; done
sleep 1
call 18141 '{"method":"set","params":{"col":"0","key":"k4","value":"v","ttl":60}}' > /dev/null
sleep 1
R1=$(expire 18141 k4 500); R2=$(expire 18142 k4 500); R3=$(expire 18143 k4 500)
sleep 1
G1=$(gttl 18141 k4); G2=$(gttl 18142 k4); G3=$(gttl 18143 k4)
N=$(( $(entries 18141) + $(entries 18142) + $(entries 18143) ))
[ "$R1" = True ] && [ "$R2" = True ] && [ "$R3" = True ] && [ "${G1:-0}" -gt 400 ] && [ "${G2:-0}" -gt 400 ] && [ "${G3:-0}" -gt 400 ] && [ "$N" = 2 ] \
	&& ok "spread: a re-arm via every node answers true, the K=2 copies moved and no extra copy appeared (ttl $G1/$G2/$G3, copies $N)" \
	|| bad "spread: re-arms $R1/$R2/$R3, ttl $G1/$G2/$G3, copies $N (want 2)"
stop_arm 4

# ---- store ---------------------------------------------------------
node 5 1 "mode = store" "" wal
node 5 2 "mode = store" "" wal
start 5 1; start 5 2
sleep 1
call 18151 '{"method":"set","params":{"col":"0","key":"k5","value":"v","ttl":60}}' > /dev/null
sleep 0.5
call 18152 '{"method":"get","params":{"col":"0","key":"k5"}}' > /dev/null   # B keeps a pulled copy
R=$(expire 18152 k5 500)
sleep 1
for p in $PIDS; do grep -qa -- "$D/n52" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done
sleep 0.5
start 5 2
sleep 1
T=$(ttl 18152 k5)
[ "$R" = True ] && [ "${T:-0}" -gt 400 ] && ok "store: a re-arm on a kept pull survived the node's restart - it is that node's record now (ttl $T)" \
	|| bad "store: after the restart the re-armed copy is gone or on the old clock (updated=$R ttl $T)"
stop_arm 5

echo "expiretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
