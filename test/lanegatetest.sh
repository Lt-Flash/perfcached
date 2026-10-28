#!/bin/sh
# lanegatetest.sh - CH-14: the frame lane's asks (LANE_FORCE, LANE_PROBE)
# are taken only from an address the node knows.  A peer whose UDP to us
# failed may be heard over the lane before its first ALIVE, so the gate is
# a slot at the address (or the master), not admission; udpblocktest is
# the flow that needs it.  Here: a two-node unicast fleet with a lane, and
# frames sealed with the fleet's secret from 127.0.141.9 (clinject).
#   1  from that unknown address: a LANE_FORCE forces nothing and a
#      LANE_PROBE draws no answer - both counted in sender_refused
#   2  the same address made known (an ALIVE from it, a free id): its
#      LANE_PROBE is answered and its LANE_FORCE forces the lane - the
#      gate tells a known sender from a stranger
# Usage: test/lanegatetest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pclgate.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23000
SECRET=lg-cluster-secret
for p in $PORT 23001 23002 23051 23052 23061 23062 23099; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "lanegatetest: port $p busy" >&2; exit 1; }
done
[ -x "$INJ" ] || { echo "lanegatetest: $INJ not built" >&2; exit 1; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = lg-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:$((23050 + $1))
http = 127.0.0.1:$((23060 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.141.1
advertise = 127.0.141.$1
max_datagram = 1400
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23060 + int(sys.argv[1])),timeout=5).read())
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
inj() { # inj <frame...> - from 127.0.141.9:23099 to node 1, a second for replies
	"$INJ" -s "$SECRET" -c "$CID" -f 127.0.141.9:23099 -t 127.0.141.1:$PORT -w 1000 "$@" 2>&1
}
refused() { st 1 'r["cluster"]["sender_refused"]'; }
forced() { st 1 'r["cluster"]["lane"]["forced"]'; }

conf 1; conf 2
start 1; sleep 2; start 2
wait_for 30 '[ "$(st 1 "r[\"cluster\"][\"peers_up\"]")$(st 2 "r[\"cluster\"][\"peers_up\"]")" = 11 ] && [ "$(st 1 "r[\"state\"]")" = ready ]' \
	|| bad "the fleet did not form: peers $(st 1 'r["cluster"]["peers_up"]')/$(st 2 'r["cluster"]["peers_up"]')"
CID=$(st 1 'r["cluster"]["cluster_id"]')

echo "--- 1: a stranger's asks"
R0=$(refused); F0=$(forced)
O=$(inj raw 24)
O2=$(inj raw 27)
sleep 0.5
[ "$(forced)" = "$F0" ] && echo "$O" | grep -q '^sent 1' \
	&& ok "LANE_FORCE from 127.0.141.9 (unknown) - nothing forced (forced $F0)" \
	|| bad "a stranger's LANE_FORCE: forced $F0 -> $(forced); $O"
echo "$O2" | grep -q '^replies 0' \
	&& ok "LANE_PROBE from it - no answer" || bad "a stranger's LANE_PROBE was answered: $O2"
[ "$(refused)" -ge $((R0 + 2)) ] 2>/dev/null \
	&& ok "both counted as refused (sender_refused $R0 -> $(refused))" \
	|| bad "sender_refused $R0 -> $(refused)"

echo "--- 2: the same address, known"
O=$(inj alive 77)
sleep 0.5                     # the ALIVE handled: a slot at 127.0.141.9
O2=$(inj raw 27)
echo "$O2" | grep -q '^reply 40 ' \
	&& ok "an ALIVE from it (node 77), then its LANE_PROBE - answered (LANE_PROBE_R)" \
	|| bad "the known sender's LANE_PROBE: $O2 (alive: $O)"
F1=$(forced)
O3=$(inj raw 24)
wait_for 3 '[ "$(forced)" -gt "$F1" ] 2>/dev/null' \
	&& ok "and its LANE_FORCE forces the lane (forced $F1 -> $(forced))" \
	|| bad "the known sender's LANE_FORCE forced nothing ($F1 -> $(forced)); $O3"

echo "lanegatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
