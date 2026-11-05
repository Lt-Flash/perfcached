#!/bin/sh
# meshtrialtest.sh - CH-14: in unicast mode an address heard only from a
# JOIN_REQ or a hint is kept CLMESH_TRIAL_MS (30 s), not ten minutes - a
# joiner re-announces every second and never lapses, while one replayed
# JOIN_REQ had bought ten minutes of every control frame at its address.
# A two-node unicast fleet; a JOIN_REQ from 127.0.144.9 (clinject) to the
# member, and then a listener on that address counting what arrives.
#   1  the fleet beats at the address it was told of: frames arrive in the
#      first seconds (node 2's beats, and the master's after node 2's hint)
#   2  nothing arrives from 70 s on: node 2 dropped it at 30 s, and the
#      master, last hinted then, 30 s after that
# The joining flows the trial must not break are foundtest's and
# unicasttest's (joiners that know each other only through hints).
# Usage: test/meshtrialtest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcmtrial.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23300
SECRET=mt-cluster-secret
for p in $PORT 23301 23302 23351 23352 23361 23362 23399; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "meshtrialtest: port $p busy" >&2; exit 1; }
done
[ -x "$INJ" ] || { echo "meshtrialtest: $INJ not built" >&2; exit 1; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = mt-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:$((23350 + $1))
http = 127.0.0.1:$((23360 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.144.1
advertise = 127.0.144.$1
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23360 + int(sys.argv[1])),timeout=5).read())
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

conf 1; conf 2
# node 1 founds alone first: which of two nodes started 2 s apart founds is
# a race (node 2 won it under ASan on GitHub), and the JOIN_REQ goes to the
# member
start 1
wait_for 30 '[ "$(st 1 "r[\"cluster\"][\"role\"]")" = master ]' || bad "node 1 did not found the fleet"
start 2
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ] && [ "$(st 1 "r[\"cluster\"][\"role\"]")" = master ]' \
	|| bad "the fleet did not form with node 1 the master: peers $(up 1)/$(up 2), role $(st 1 'r["cluster"]["role"]')"
CID=$(st 1 'r["cluster"]["cluster_id"]')

echo "--- a JOIN_REQ from 127.0.144.9 to member node 2, then a listener there"
O=$("$INJ" -s "$SECRET" -c "$CID" -f 127.0.144.9:23399 -t 127.0.144.2:$PORT joinreq 2>&1)
echo "$O" | grep -q '^sent 1' || bad "the JOIN_REQ was not sent: $O"
python3 - > "$D/arrivals" <<'EOF'
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.144.9", 23399))
s.settimeout(0.5)
t0 = time.time()
while time.time() - t0 < 81:
    try:
        s.recv(65535)
        print("%.1f" % (time.time() - t0))
    except socket.timeout:
        pass
EOF
EARLY=$(awk '$1 >= 1 && $1 < 12' "$D/arrivals" | wc -l)
LATE=$(awk '$1 >= 70 && $1 < 81' "$D/arrivals" | wc -l)
[ "$EARLY" -ge 5 ] \
	&& ok "1. the fleet beats at the address it was told of ($EARLY frames in its first 11 s)" \
	|| bad "1. only $EARLY frames in the first 11 s - the address was not learned"
[ "$LATE" -eq 0 ] \
	&& ok "2. nothing from 70 s on - learned from a JOIN_REQ and a hint only, it lapsed after its trial" \
	|| bad "2. $LATE frames between 70 and 81 s - still beating at an address no admitted node named (last at $(tail -1 "$D/arrivals") s)"

echo "meshtrialtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
