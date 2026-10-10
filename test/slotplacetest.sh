#!/bin/sh
# slotplacetest.sh — the daemon and libperfd must agree, key for key,
# about which node owns a key.
#
# Placement moved from a key hash of ours to the Redis SLOT
# (crc16(key) % 16384) so that a RESP client can compute it too (S44).
# Two things now derive an owner independently: pc_shard_owner() in the
# daemon and route_owner() in libperfd.  If they ever diverge, a key
# lives on one node and is looked for on another - the exact failure
# clplace.h warns about - and nothing else in the suite would catch it.
#
# The assertion needs no reimplementation of the hash: a ROUTED client
# that agrees with the daemon never causes a forward, so the daemon's
# own fwd_sent counter is the oracle.  Any disagreement, on any key,
# shows up as a non-zero count.
#
# Usage: test/slotplacetest.sh [./perfcached] [./natbench] [./perfcli]
set -u

BIN=${1:-./perfcached}
NAT=${2:-./natbench}
CLI=${3:-./perfcli}
D=$(mktemp -d /var/tmp/pcslot.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; \
     [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

[ -x "$NAT" ] || { echo "slotplacetest: $NAT not built - skipping"; exit 0; }

node() { # node <n> <cliport>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = sl-client-secret
cluster = sl-cluster-secret
[listen]
# listen ON the advertised address, not on 127.0.0.1.  A ROUTING client
# dials each member at the address the fleet advertises, so a test where
# the two differ has every routed connection refused - which looks like
# "routing is broken" and is really "the address was never dialable".
tcp = 127.0.0.6$1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.44:17144
advertise = 127.0.0.6$1
pull_timeout_ms = 300
mode = shard
collections = sl
[collection sl]
buckets_log2 = 12
EOF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
stat_of() { # stat_of <host> <port> <field>
	"$CLI" -h "$1" -p "$2" \
		-j '{"method":"stats"}' 2>/dev/null |
		tr ',' '\n' | grep -o "\"$3\":[0-9]*" | head -1 | cut -d: -f2
}

node 1 17441; node 2 17442; node 3 17443
start 1; start 2; start 3

i=0; peers=0
while [ $i -lt 40 ]; do
	peers=$("$CLI" -h 127.0.0.61 -p 17441 \
		-j '{"method":"members"}' 2>/dev/null | grep -o '"node"' | wc -l)
	[ "${peers:-0}" -ge 3 ] && break
	sleep 0.5; i=$((i+1))
done
if [ "${peers:-0}" -lt 3 ]; then
	echo "  FAIL membership never formed ($peers of 3) - nothing below"
	echo "       would mean anything"
	echo "slotplacetest: 0 passed, 1 failed"
	exit 1
fi
ok "membership formed (3 nodes)"

# the daemon must advertise the algorithm the client implements, or the
# client turns routing off and the test silently measures nothing
algo=$("$CLI" -h 127.0.0.61 -p 17441 \
	-j '{"method":"members"}' 2>/dev/null |
	grep -o '"algo":"[^"]*"' | head -1 | cut -d'"' -f4)
[ "$algo" = "hrw-slot16k-v1" ] && ok "advertised algo is $algo" \
	|| bad "advertised algo is '$algo', want hrw-slot16k-v1"

f0=0
for n in 1 2 3; do
	v=$(stat_of 127.0.0.6$n 1744$n fwd_sent); f0=$((f0 + ${v:-0}))
done

# a ROUTED client, writing then reading across the whole keyspace
out=$("$NAT" 127.0.0.61 17441 - sl 12 3 8 3 5000 64 0 1 0 -1 1 2>&1)
routed=$(printf '%s' "$out" | grep -o 'routing: [0-9]*' | head -1 | cut -d' ' -f2)
"$NAT" 127.0.0.61 17441 - sl 12 3 8 3 5000 64 100 1 0 -1 1 >/dev/null 2>&1

# routing must actually have engaged, or fwd_sent is trivially 0
if [ "${routed:-0}" -gt 0 ]; then
	ok "client routing engaged ($routed connections)"
else
	bad "client did not route - fwd_sent below would prove nothing"
fi

f1=0
for n in 1 2 3; do
	v=$(stat_of 127.0.0.6$n 1744$n fwd_sent); f1=$((f1 + ${v:-0}))
done
fwd=$((f1 - f0))
if [ "$fwd" -eq 0 ]; then
	ok "no forwards: client and daemon agree on every key"
else
	bad "$fwd forward(s) - the client and the daemon DISAGREE about"
	bad "  ownership for at least one key; placement has diverged"
fi

# and the data really is spread, not all on the ingress
n1=$(stat_of 127.0.0.61 17441 entries)
n2=$(stat_of 127.0.0.62 17442 entries)
n3=$(stat_of 127.0.0.63 17443 entries)
if [ "${n1:-0}" -gt 0 ] && [ "${n2:-0}" -gt 0 ] && [ "${n3:-0}" -gt 0 ]; then
	ok "keys spread over all three nodes ($n1/$n2/$n3)"
else
	bad "keys not spread ($n1/$n2/$n3) - slot placement is not distributing"
fi

echo "slotplacetest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
