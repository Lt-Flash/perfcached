#!/bin/sh
# replaytest.sh - CH-1: a sealed cluster frame is taken once.
#
# A forwarded write carries no request record at the holder, so a datagram
# the network doubled - or one captured and sent again - applied twice: an
# INCR counted twice.  Every frame now names its sender, the sealing
# thread's stream and its place in it, inside the authenticated header.
# One node; clinject plays a member (its ALIVE first, CH-12) and sends ONE
# sealed forwarded INCR twice:
#  - the counter moves once, and /stats cluster.replay.dup counts the copy;
#  - the same INCR sealed again by a new process (a new incarnation, its
#    sequence from 1) is a new frame and is applied - nothing keys on the
#    content.
# Fail-first: before CH-1 the copy is applied, the counter moves twice.
# Usage: test/replaytest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcrp.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=24800
SECRET=rp-cluster-secret
[ -x "$INJ" ] || { echo "replaytest: $INJ not built" >&2; exit 1; }

cat > "$D/n1.conf" <<EOF
[daemon]
workers = 1
log_level = info
state_dir = $D/s1
[memory]
arena_mb = 32
[secrets]
client = rp-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:24851
http = 127.0.0.1:24861
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.157.1
advertise = 127.0.157.1
[collection 0]
buckets_log2 = 10
mode = store
EOF
chmod 600 "$D/n1.conf"; mkdir -p "$D/s1"
st() { # st <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:24861/stats",timeout=5).read())
print(eval(sys.argv[1]))' "$1" 2>/dev/null || echo "?"
}
call() { # call <json>
	python3 -c 'import json,pcnative,socket,sys
s=socket.create_connection(("127.0.0.1", 24851), 5); f=pcnative.wrap(s)
f.write(sys.argv[1].encode()+b"\n"); f.flush(); print(json.dumps(json.loads(f.readline()).get("result")))' "$1" 2>/dev/null
}
ctr() { call '{"method":"get","params":{"col":"0","key":"ctr"}}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin) or {}).get("value"))
except Exception: print("?")'; }
inj() { "$INJ" -s "$SECRET" -c "$CID" -f 127.0.157.9:24899 -t 127.0.157.1:$PORT -w 500 "$@" 2>&1; }

"$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 & echo $! > "$D/n1.pid"
k=0
while [ $k -lt 60 ] && [ "$(st 'r["state"]')" != ready ]; do sleep 0.5; k=$((k+1)); done
[ "$(st 'r["state"]')" = ready ] || { bad "the node did not start: $(tail -2 "$D/n1.log")"; echo "replaytest: $pass passed, $fail failed"; exit 1; }
CID=$(st 'r["cluster"]["cluster_id"]')
call '{"method":"set","params":{"col":"0","key":"ctr","value":"10"}}' > /dev/null
inj alive 88 > /dev/null
sleep 0.5

O=$(inj -n 2 fwdadd 0 ctr 5)
sleep 0.5
V=$(ctr); DUP=$(st 'r["cluster"]["replay"]["dup"]')
[ "$V" = 15 ] && [ "$DUP" -ge 1 ] 2>/dev/null \
	&& ok "one sealed INCR of 5, sent twice: the counter went 10 -> 15 once, and the copy counted (replay.dup $DUP)" \
	|| bad "the INCR sent twice: counter $V (want 15), replay.dup $DUP; clinject: $(echo "$O" | tr '\n' ' ' | cut -c1-80)"
grep -q "its sequence was taken before" "$D/n1.log" \
	&& ok "and the log says why: $(grep -m1 'its sequence was taken before' "$D/n1.log" | sed 's/.*cluster: //' | cut -c1-70)" \
	|| bad "no log line for the dropped copy"

inj fwdadd 0 ctr 5 > /dev/null
sleep 0.5
V=$(ctr)
[ "$V" = 20 ] && ok "the same INCR sealed by a new process is a new frame: 15 -> 20" \
	|| bad "a new process's INCR: counter $V (want 20)"

echo "replaytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
