#!/bin/sh
# dnsseedtest.sh - S36 step a: seeds are DNS NAMES, re-resolved while
# the node runs - a cloud instance comes back at a new address.
#
# A network namespace of its own, whose /etc/hosts `ip netns exec`
# swaps in (/etc/netns/<ns>/hosts), so a name can be pointed at one
# node and then moved to another without touching the host:
#   1  nodes 1 and 2 form a fleet through `seeds = seed-a`, seed-a ->
#      node 1 (node 1 has no seeds)
#   2  node 1 dies; seed-a moves to node 3's address; node 3 starts blank
#      with NO seeds.  Node 2 - which only ever knew seed-a by name -
#      re-resolves it, beats at node 3, and node 3 (a founder seconds
#      old) yields to node 2's established cluster: one fleet, node 2's
#      id, inside the 30 s provisional window
# Needs root and ip netns; SKIPs loudly without them - a skip is not a
# pass.
# Usage: test/dnsseedtest.sh [./perfcached]
set -u
BIN=$(readlink -f "${1:-./perfcached}")
if [ "$(id -u)" != 0 ] || ! command -v ip >/dev/null 2>&1; then
	echo "dnsseedtest: SKIP - needs root and ip netns - nothing was tested"
	exit 0
fi
NS=pcdns$$
D=$(mktemp -d /var/tmp/pcdns.XXXXXX)
cleanup() {
	for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done
	ip netns del $NS 2>/dev/null
	rm -rf "/etc/netns/$NS" "$D"
}
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=17840

ip netns add $NS || { echo "dnsseedtest: cannot make a namespace"; exit 1; }
ip -n $NS link set lo up
mkdir -p "/etc/netns/$NS"
hosts() { printf '127.0.0.1 localhost\n%s seed-a\n' "$1" > "/etc/netns/$NS/hosts"; }
hosts 127.0.33.1

conf() { # conf <n> <seeds>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = dn-client-secret
cluster = dn-cluster-secret
[listen]
tcp = 127.0.0.1:$((17850 + $1))
http = 127.0.0.1:$((17860 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = $2
advertise = 127.0.33.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() {
	ip netns exec $NS "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
}
st() { # st <n> <expr over r>
	ip netns exec $NS python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (17860 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
cid() { st "$1" 'r["cluster"]["cluster_id"]'; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

echo "--- 1: a fleet found through a seed NAME"
conf 1 ""; conf 2 seed-a
start 1; sleep 1; start 2
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ] && [ "$(cid 1)" = "$(cid 2)" ]' \
	&& ok "nodes 1 and 2 formed through seeds = seed-a ($(cid 2))" \
	|| bad "no fleet through the name (peers $(up 1)/$(up 2))"
[ "$(st 2 'r["cluster"]["discovery"]["seeds_resolved"]')" = 1 ] && ok "node 2 resolved its one seed" \
	|| bad "node 2's seeds_resolved is $(st 2 'r["cluster"]["discovery"]["seeds_resolved"]')"
C2=$(cid 2)

echo "--- 2: the name moves; the running node follows it"
kill -9 "$(cat "$D/n1.pid")"; rm -f "$D/n1.pid"
hosts 127.0.33.3
wait_for 20 '[ "$(up 2)" = 0 ]' || true
conf 3 ""
start 3
wait_for 40 '[ "$(up 2)$(up 3)" = 11 ] && [ "$(cid 3)" = "$C2" ]' \
	&& ok "node 2 re-resolved seed-a to node 3, and node 3 joined node 2's cluster ($C2)" \
	|| bad "node 3 not in node 2's fleet (peers $(up 2)/$(up 3), ids $(cid 2) / $(cid 3))"
grep -q "yielding to node" "$D/n3.log" && ok "node 3 yielded its seconds-old founding to the established cluster" \
	|| { [ "$(cid 3)" = "$C2" ] && ok "node 3 joined without founding first" || bad "node 3 never yielded"; }

grep -hE "ERROR|CRIT" "$D"/n*.log | head -5
echo "dnsseedtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
