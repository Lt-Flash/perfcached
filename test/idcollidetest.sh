#!/bin/sh
# idcollidetest.sh - CH-12: an ALIVE claiming an id a live member at
# another address holds is counted and said, not refused.  Two halves of
# a split can each admit a joiner under one id, and that already ends in a
# map no node decodes; refusing would only make the nodes disagree in
# silence.  A forged claim looks the same, so the suite plays one: an
# ALIVE from 127.0.143.9 (clinject) claiming node 2's id.
#   1  node 1 counts it (id_collisions) and names both addresses in its log
#   2  the fleet is whole again once the false claim's slot has aged out
# Usage: test/idcollidetest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcidcol.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23200
SECRET=ic-cluster-secret
for p in $PORT 23201 23202 23251 23252 23261 23262 23299; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "idcollidetest: port $p busy" >&2; exit 1; }
done
[ -x "$INJ" ] || { echo "idcollidetest: $INJ not built" >&2; exit 1; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = ic-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:$((23250 + $1))
http = 127.0.0.1:$((23260 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.143.1
advertise = 127.0.143.$1
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23260 + int(sys.argv[1])),timeout=5).read())
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
ready() { st "$1" 'r["state"]'; }

conf 1; conf 2
start 1; sleep 2; start 2
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ] && [ "$(ready 1)$(ready 2)" = readyready ]' \
	|| bad "the fleet did not form: peers $(up 1)/$(up 2)"
CID=$(st 1 'r["cluster"]["cluster_id"]')
N2=$(st 2 'r["cluster"]["node"]')

echo "--- 1: node $N2's id claimed from 127.0.143.9"
C0=$(st 1 'r["cluster"]["id_collisions"]')
O=$("$INJ" -s "$SECRET" -c "$CID" -f 127.0.143.9:23299 -t 127.0.143.1:$PORT alive "$N2" 2>&1)
wait_for 3 '[ "$(st 1 "r[\"cluster\"][\"id_collisions\"]")" -gt "$C0" ] 2>/dev/null' \
	&& ok "node 1 counted it (id_collisions $C0 -> $(st 1 'r["cluster"]["id_collisions"]'))" \
	|| bad "id_collisions $C0 -> $(st 1 'r["cluster"]["id_collisions"]') ($O)"
grep -q "node $N2 is claimed by 127.0.143.2 and by 127.0.143.9" "$D/n1.log" \
	&& ok "and said who holds it and who claims it" \
	|| bad "no collision line: $(grep -h 'claimed by' "$D/n1.log" | tail -1 | cut -c1-160)"

echo "--- 2: the fleet afterwards"
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ] && [ "$(ready 1)$(ready 2)" = readyready ] && [ "$(st 2 "r[\"cluster\"][\"map\"][\"valid\"]")" = True ]' \
	&& ok "the false claim aged out: one fleet of two, both ready, node 2 holds a map" \
	|| bad "after the claim: peers $(up 1)/$(up 2), states $(ready 1)/$(ready 2), node 2 map $(st 2 'r["cluster"]["map"]["valid"]')"

echo "idcollidetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
