#!/bin/sh
# lamporttest.sh — a version that works without a synced clock.
#
# Nothing in the cluster could say which of two copies of a key was
# newer.  A wall-clock stamp would answer it only if every node's clock
# agreed, and this design refuses that dependency on purpose: the pull
# path ships TTL as REMAINING seconds and the receiver rebases it, so two
# nodes never have to agree on the time.
#
# A Lamport counter needs no clock.  Asserted here:
#  - it advances on writes, not on time passing;
#  - it CONVERGES across the fleet via the heartbeat, so a node that
#    never wrote anything still knows roughly where the cluster is;
#  - a node restarted from nothing LEARNS the fleet value instead of
#    starting at 0 - otherwise every restart would claim to be oldest and
#    its writes would lose to stale copies for ever.
# usage: test/lamporttest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pclam.XXXXXX)
SEC=lm-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT

for pf in 17971 17972 17973; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "lamporttest: port $pf already bound" >&2; exit 1; }
done

mk() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = lm-cluster-secret
[listen]
tcp = 127.0.51.$1:1797$1
[cluster]
multicast = 239.255.77.159:17259
advertise = 127.0.51.$1
pull_timeout_ms = 400
mode = store
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
lam() { ./perfcli -h 127.0.51.$1 -p 1797$1 -a $SEC -j '{"method":"stats"}' \
	2>/dev/null | python3 -c 'import json,sys
try: print(json.load(sys.stdin)["cluster"]["lamport"])
except Exception: print("ERR")'; }
put() { ./perfcli -h 127.0.51.$1 -p 1797$1 -a $SEC \
	-j "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"$2\",\"value\":\"v\"}}" \
	>/dev/null 2>&1; }

mk 1; mk 2; mk 3
start 1 a && start 2 a && start 3 a || { echo "fleet did not start"; exit 1; }
sleep 6

# it advances with WRITES, not with time
A0=$(lam 1)
sleep 3
A1=$(lam 1)
[ "$A0" = "$A1" ] && ok "idle time does not advance the clock ($A0)" \
	|| echo "  ..   note: clock moved while idle ($A0 -> $A1) - peers writing?"
i=0; while [ $i -lt 50 ]; do put 1 "lk$i"; i=$((i+1)); done
A2=$(lam 1)
[ "$A2" -ge $((A1 + 50)) ] && ok "50 writes advanced it by >= 50 ($A1 -> $A2)" \
	|| bad "writes did not advance the clock ($A1 -> $A2)"

# and it CONVERGES: a node that wrote nothing still tracks the fleet
i=0
while [ $i -lt 20 ]; do
	B=$(lam 2); C=$(lam 3)
	[ "$B" != "ERR" ] && [ "$B" -ge "$A2" ] && [ "$C" -ge "$A2" ] && break
	sleep 1; i=$((i+1))
done
B=$(lam 2); C=$(lam 3)
echo "  ..   after 50 writes on node1: n1=$A2 n2=$B n3=$C"
[ "$B" -ge "$A2" ] && [ "$C" -ge "$A2" ] \
	&& ok "idle peers converged to the writer's clock via the heartbeat" \
	|| bad "peers did not converge (n1=$A2 n2=$B n3=$C)"

# a node restarted from nothing must LEARN the fleet value, not restart at 0
pkill -9 -f "[p]erfcached -f $D/n3.conf"; sleep 8
start 3 b || bad "node3 did not restart"
sleep 6
C2=$(lam 3)
echo "  ..   node3 after a from-scratch restart: $C2"
[ "$C2" != "ERR" ] && [ "$C2" -ge "$A2" ] \
	&& ok "a restarted node learned the fleet clock instead of starting at 0" \
	|| bad "a restarted node came back behind the fleet ($C2 < $A2)"

echo "lamporttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
