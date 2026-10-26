#!/bin/sh
# datagatetest.sh - CH-12/CH-14: the data plane (pulls, forwards,
# migrations, replication, tombstones, publishes, demotes) is taken from an
# address the node knows.  A stranger's PULL_REQ drew the value back to the
# address it named; S302's bounce still has its slot, and a node just
# admitted announces itself to every member before its first pull.  A
# three-node unicast fleet, collection 0 stored where written and pulled
# on a miss; frames from 127.0.142.9 sealed with the fleet's secret.
#   1  node 3 joins last, seeded with node 1 only; read on node 3 the moment
#      it knows both peers, a key only node 2 holds is pulled from node 2 -
#      the join's own data plane is not refused
#   2  a PULL_REQ for that key from 127.0.142.9 (unknown) draws no reply,
#      counted in sender_refused
#   3  the same address made known (an ALIVE from it): its PULL_REQ is
#      answered - the gate tells a known sender from a stranger
# Usage: test/datagatetest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcdgate.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23100
SECRET=dg-cluster-secret
for p in $PORT 23101 23102 23151 23152 23153 23161 23162 23163 23199; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "datagatetest: port $p busy" >&2; exit 1; }
done
[ -x "$INJ" ] || { echo "datagatetest: $INJ not built" >&2; exit 1; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = dg-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:$((23150 + $1))
http = 127.0.0.1:$((23160 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.142.1
advertise = 127.0.142.$1
[collection 0]
buckets_log2 = 10
mode = store
pull = 1
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23160 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
call() { # call <n> <json>
	python3 -c 'import json,pcnative,socket,sys
s=socket.create_connection(("127.0.0.1", 23150 + int(sys.argv[1])), 5); f=pcnative.wrap(s)
f.write(sys.argv[2].encode()+b"\n"); f.flush(); print(json.dumps(json.loads(f.readline()).get("result")))' "$1" "$2" 2>/dev/null
}
getv() { call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"0\",\"key\":\"$2\"}}"; }
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
inj() { # inj <frame...> - from 127.0.142.9:23199 to node 2, a second for replies
	"$INJ" -s "$SECRET" -c "$CID" -f 127.0.142.9:23199 -t 127.0.142.2:$PORT -w 1000 "$@" 2>&1
}

echo "--- 1: a joiner's pull, at once"
conf 1; conf 2; conf 3
start 1; sleep 2; start 2
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ] && [ "$(ready 2)" = ready ]' \
	|| bad "nodes 1 and 2 did not form: peers $(up 1)/$(up 2)"
CID=$(st 1 'r["cluster"]["cluster_id"]')
W=$(call 2 '{"method":"set","params":{"col":"0","key":"dk","value":"held-by-2"}}')
start 3
# ready AND knowing node 2: seeded with node 1 only, a node 3 that has not
# heard of node 2 yet asks node 1 alone and is told "absent" - which says
# nothing about the gate (GitHub 40074218338, v0.7.2-rc1: null, once in
# many runs).  What is under test is node 2 taking the pull of a node it
# admitted moments ago
wait_for 30 '[ "$(ready 3)" = ready ] && [ "$(up 3)" = 2 ]' \
	|| bad "node 3 did not join (state $(ready 3), peers $(up 3))"
R2=$(st 2 'r["cluster"]["sender_refused"]')
V=$(getv 3 dk)
echo "$V" | grep -q '"held-by-2"' \
	&& ok "node 3, read the moment it knows both peers: node 2's key pulled (${V})" \
	|| bad "node 3 could not pull node 2's key right after joining: $V (set: $W; node 3 peers $(up 3), node 2 sender_refused $R2 -> $(st 2 'r["cluster"]["sender_refused"]'))"

echo "--- 2: a stranger's pull"
R0=$(st 2 'r["cluster"]["sender_refused"]')
O=$(inj pullreq 0 dk)
echo "$O" | grep -q '^replies 0' \
	&& ok "PULL_REQ for dk from 127.0.142.9 (unknown) - no reply" \
	|| bad "a stranger's PULL_REQ was answered: $O"
[ "$(st 2 'r["cluster"]["sender_refused"]')" -gt "$R0" ] 2>/dev/null \
	&& ok "counted as refused (sender_refused $R0 -> $(st 2 'r["cluster"]["sender_refused"]'))" \
	|| bad "sender_refused $R0 -> $(st 2 'r["cluster"]["sender_refused"]')"

echo "--- 3: the same address, known"
inj alive 88 > /dev/null
sleep 0.5
O=$(inj pullreq 0 dk)
echo "$O" | grep -q '^reply 3 ' \
	&& ok "an ALIVE from it (node 88), then its PULL_REQ - answered (PULL_RSP)" \
	|| bad "the known sender's PULL_REQ: $O"

echo "datagatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
