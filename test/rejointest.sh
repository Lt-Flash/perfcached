#!/bin/sh
# rejointest.sh — what a node that REJOINS with a WAL is owed, and what
# it must not be given.
#
# A node with a WAL comes back holding an unknown subset of the keyspace,
# and the fleet cannot see which keys or how many.  Two things must hold
# before any reconciliation protocol is designed on top, and both are
# about data the node itself is wrong or right about:
#
#   1. A value it AUTHORED and logged but never replicated is NEWER than
#      the fleet's copy.  Recovery must keep it - no peer may push an
#      older copy over it.
#   2. A key DELETED while it was down must not come back.  Its WAL still
#      has the record; the tombstone happened where it could not see it.
#
# usage: test/rejointest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrj.XXXXXX)
SEC=rj-client-secret
P1= P2= P3=
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done; rm -rf "$D"' EXIT TERM INT

for pf in 17951 17952 17953; do
	if ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]"; then
		echo "rejointest: port $pf already bound" >&2; exit 1
	fi
done

mk() {
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = rj-cluster-secret
[listen]
tcp = 127.0.48.$1:1795$1
[cluster]
multicast = 239.255.77.153:17253
advertise = 127.0.48.$1
pull_timeout_ms = 400
mode = store
eager = 1
collections = c
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
fsync = always
save = off
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.$2.log"; exit 1
}
call() { # call <node> <json>
	printf '%s\n' "$2" | timeout 10 ./perfcli -h 127.0.48.$1 -p 1795$1 \
		-a $SEC -j "$2" 2>/dev/null
}
getv() { call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}"; }

for i in 1 2 3; do mk $i; done
for i in 1 2 3; do start $i a; done
sleep 5

# ---- 1: a value authored and logged but never replicated ---------------
call 1 '{"method":"set","params":{"col":"c","key":"k1","value":"V1"}}' >/dev/null
i=0
while [ $i -lt 40 ]; do
	echo "$(getv 2 k1)" | grep -q V1 && break
	sleep 1; i=$((i+1))
done
echo "$(getv 2 k1)" | grep -q V1 && ok "peers hold V1 before the kill" \
	|| bad "peers never got V1"

# author V2 and pull the plug before the sweep can push it.  fsync=always
# so the log has it; the sweep tick is ~10s so it cannot have run.
call 1 '{"method":"set","params":{"col":"c","key":"k1","value":"V2"}}' >/dev/null
sleep 1
kill -9 $P1 2>/dev/null; P1=
R=$(getv 2 k1)
echo "$R" | grep -q V1 && ok "peers still hold the OLD V1 (V2 never replicated)" \
	|| echo "  ..   note: peers already had V2 - test cannot discriminate: $R"

sleep 8
start 1 b
R=$(getv 1 k1)
echo "  ..   node1 straight after recovery: $R"
echo "$R" | grep -q V2 && ok "recovery restored the newer V2" \
	|| bad "recovery did not restore V2: $R"
sleep 25                      # let the fleet do whatever it is going to do
R=$(getv 1 k1)
echo "  ..   node1 after the fleet settled: $R"
echo "$R" | grep -q V2 && ok "OBLIGATION 1: peers did not overwrite the newer value" \
	|| bad "OBLIGATION 1: a peer's older copy overwrote the recovered V2: $R"
# ...and prove WHY, rather than trusting a race: a node that came back
# holding data must be refused a blind backfill, and say so.
if grep -q "NOT backfilling" "$D"/n2.a.log "$D"/n3.a.log 2>/dev/null; then
	ok "the gate fired: a peer refused to backfill a node holding data"
else
	bad "no peer logged the backfill refusal - obligation 1 may have \
passed by luck rather than by the gate"
fi

# ---- 3: a key deleted while the node was down --------------------------
call 1 '{"method":"set","params":{"col":"c","key":"k2","value":"D1"}}' >/dev/null
i=0
while [ $i -lt 40 ]; do
	echo "$(getv 2 k2)" | grep -q D1 && break
	sleep 1; i=$((i+1))
done
echo "$(getv 2 k2)" | grep -q D1 && ok "k2 replicated before the kill" \
	|| bad "k2 never replicated"
kill -9 $P1 2>/dev/null; P1=
sleep 8
call 2 '{"method":"del","params":{"col":"c","key":"k2"}}' >/dev/null
sleep 2
echo "$(getv 2 k2)" | grep -qE '"found":[[:space:]]*false' \
	&& ok "k2 deleted on the survivors" \
	|| bad "k2 not deleted on node2"
start 1 c
sleep 20
R=$(getv 1 k2)
echo "  ..   node1 on k2 after rejoin: $R"
echo "$R" | grep -qE '"found":[[:space:]]*false' \
	&& ok "OBLIGATION 3: the deleted key did not come back" \
	|| bad "OBLIGATION 3: WAL replay RESURRECTED a key deleted while the node was down: $R"

echo "rejointest: $pass passed, $fail failed"
[ $fail -eq 0 ]
