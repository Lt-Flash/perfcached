#!/bin/sh
# foundtest.sh - S36 step c: who founds a cluster on a BLANK cold start
# in discovery = unicast, so a cold start ends as one fleet.
#
# A blank node (no cluster id on disk, none pinned) founds only when the
# joiners it hears have been stable 5 s, it sees `[cluster] expect`
# blank nodes (itself included), and no live joiner has a higher
# address.  A joining node shares the addresses it learns with every
# address it knows, so nodes seeded only with a third still find each
# other.
#   1  five blank nodes, every node seeded with all five (a headless
#      Service's answer), started together: one cluster, one id - three
#      rounds
#   2  expect = 3 and only two nodes up for 20 s: neither founds; the
#      third arrives and the three form one fleet
#   3  expect = 3, nodes 2 and 3 seeded ONLY with node 1, which starts
#      40 s later - past S302's 30 s provisional window, where two lone
#      founders would have split for good: one fleet of three
# Usage: test/foundtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfnd.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18040

conf() { # conf <n> <seeds> <expect>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 16
[secrets]
client = fd-client-secret
cluster = fd-cluster-secret
[listen]
tcp = 127.0.0.1:$((18050 + $1))
http = 127.0.0.1:$((18060 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = $2
expect = $3
advertise = 127.0.34.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; rm -rf "$D/s$1"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
stopall() { for f in "$D"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; rm -f "$f"; done; sleep 1; }
st() {
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18060 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
cid() { st "$1" 'r["cluster"]["cluster_id"]'; }
ids() { for n in "$@"; do cid $n; done | sort -u | grep -c .; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
ALL5="127.0.34.1,127.0.34.2,127.0.34.3,127.0.34.4,127.0.34.5"

echo "--- 1: five blank nodes, all seeded with all five, started together"
for round in 1 2 3; do
	for n in 1 2 3 4 5; do conf $n "$ALL5" 0; done
	for n in 1 2 3 4 5; do start $n; done
	if wait_for 40 '[ "$(up 1)$(up 2)$(up 3)$(up 4)$(up 5)" = 44444 ] && [ "$(ids 1 2 3 4 5)" = 1 ]'; then
		ok "round $round: one cluster of five, one id"
	else
		bad "round $round: peers $(up 1)/$(up 2)/$(up 3)/$(up 4)/$(up 5), $(ids 1 2 3 4 5) distinct id(s)"
	fi
	F=$(grep -l "founded cluster" "$D"/n*.log 2>/dev/null | wc -l)
	echo "    $F node(s) minted an id (founders that yielded inside the window count)"
	stopall; rm -f "$D"/n*.log
done

echo "--- 2: expect = 3, two of three up"
for n in 1 2 3; do conf $n "127.0.34.1,127.0.34.2,127.0.34.3" 3; done
start 1; start 2
sleep 20
[ -z "$(cid 1)$(cid 2)" ] && ! grep -q "founded cluster" "$D/n1.log" "$D/n2.log" \
	&& ok "after 20 s neither of the two founded (no cluster id)" \
	|| bad "a node founded below expect (ids '$(cid 1)' '$(cid 2)')"
grep -q "fewer blank nodes than \[cluster\] expect" "$D/n1.log" "$D/n2.log" \
	&& ok "and they say why" || bad "no 'fewer blank nodes than expect' line"
start 3
wait_for 40 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(ids 1 2 3)" = 1 ]' \
	&& ok "the third arrived: one fleet of three, one id" \
	|| bad "no fleet after the third (peers $(up 1)/$(up 2)/$(up 3), $(ids 1 2 3) id(s))"
stopall; rm -f "$D"/n*.log

echo "--- 3: expect = 3, nodes 2 and 3 seeded only with node 1, which comes 40 s late"
conf 1 "" 3; conf 2 127.0.34.1 3; conf 3 127.0.34.1 3
start 2; start 3
sleep 40
start 1
wait_for 40 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(ids 1 2 3)" = 1 ]' \
	&& ok "one fleet of three, one id - nodes 2 and 3 waited, learned each other through node 1" \
	|| bad "no single fleet (peers $(up 1)/$(up 2)/$(up 3), $(ids 1 2 3) id(s))"
stopall

grep -hE "ERROR|CRIT" "$D"/n*.log 2>/dev/null | head -5
echo "foundtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
