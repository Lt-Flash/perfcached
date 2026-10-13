#!/bin/sh
# replicastest.sh - S157: a spread fleet whose live members fall below
# `replicas` says so - one WARNING in, one NOTICE out, replicas_short on
# /stats - and a fleet that can still honour K says nothing.
# Usage: test/replicastest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcreplicas.XXXXXX); P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n> <replicas>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 1
log_level = info
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = replicas-client-secret
cluster = replicas-cluster-secret
[listen]
tcp = 127.0.0.1:1746$1
http = 127.0.0.1:1846$1
plaintext = loopback
[cluster]
multicast = 239.255.77.48:17148
advertise = 127.0.11.$1
mode = spread
replicas = $2
collections = b
[collection b]
buckets_log2 = 12
CONF
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; return 1; }
short() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1846%s/stats" % sys.argv[1], timeout=4))
    c = d["cluster"]; print("%s %s %s" % (d.get("state"), c.get("peers_up"), c.get("replicas_short", "MISSING")))
except Exception as e:
    print("? ? ?")' "$1" 2>/dev/null; }
wait_short() { # wait_short <node> <want> <secs>
	i=0; while [ $i -lt "$3" ]; do set -- "$1" "$2" "$3" $(short "$1"); [ "$6" = "$2" ] && return 0; sleep 1; i=$((i+1)); done; return 1; }
fleet() { # fleet <replicas>
	for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; P1= P2= P3=; rm -rf "$D"/s? "$D"/n?.log
	node 1 "$1"; node 2 "$1"; node 3 "$1"; start 1 && start 2 && start 3 || exit 1
	i=0; while [ $i -lt 150 ]; do set -- "$1" $(short 1); [ "$2" = ready ] && [ "$3" = 2 ] && return 0; sleep 0.2; i=$((i+1)); done
	echo "  fleet never formed: $(short 1)"; exit 1; }

echo "== three nodes at replicas = 3"
fleet 3
set -- $(short 1); [ "$3" != MISSING ] && ok "/stats carries replicas_short (reads $3)" || bad "/stats has no replicas_short field"
wait_short 1 0 10 && ok "a whole fleet reads replicas_short 0" || bad "a whole fleet reads replicas_short $(short 1 | cut -d' ' -f3)"
kill -9 "$P3"; wait "$P3" 2>/dev/null; P3=
wait_short 1 1 75 && ok "one node down: replicas_short 1 on a survivor (after the slot grace)" || bad "one node down: replicas_short stayed $(short 1 | cut -d' ' -f3) for 75 s"
grep -q "replicas = 3 with 2 live member(s): every key has at most 2 copies" "$D/n1.log" && ok "the WARNING names K, the live count and the copies a key can have" || bad "no WARNING on node 1: $(grep -c replicas $D/n1.log) 'replicas' line(s)"
[ "$(grep -c 'replicas = 3 with 2 live' "$D/n1.log")" = 1 ] && ok "and it was logged once" || bad "the WARNING was logged $(grep -c 'replicas = 3 with 2 live' $D/n1.log) times"
start 3 || exit 1
wait_short 1 0 45 && ok "the node back: replicas_short 0 again" || bad "the node back: replicas_short still $(short 1 | cut -d' ' -f3) after 45 s"
grep -q "replicas = 3 met again by 3 live members" "$D/n1.log" && ok "and the NOTICE says it is met again" || bad "no NOTICE when the fleet was whole again"

echo "== negative control: three nodes at replicas = 2 lose one"
fleet 2
kill -9 "$P3"; wait "$P3" 2>/dev/null; P3=
sleep 45
set -- $(short 1); [ "$3" = 0 ] && ok "K=2 with two live members: replicas_short 0" || bad "K=2 with two live members reads replicas_short $3"
grep -q "replicas = 2 with" "$D/n1.log" && bad "a fleet that can still honour K=2 warned" || ok "and no WARNING was logged"
echo "replicastest: $pass passed, $fail failed"
[ $fail -eq 0 ]
