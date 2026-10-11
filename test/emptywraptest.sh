#!/bin/sh
# emptywraptest.sh - S334: an EMPTY node restarted more times than its WAL
# has segments comes back READY, not FAILED.
#
# Every start activates the next WAL generation, round the segment ring.
# pc_wal_init refuses to activate a segment that still holds records newer
# than the last completed snapshot - correct - but it read that marker
# before this process had completed a snapshot, and the post-recovery
# checkpoint that sets it runs only when recovery brought records back.
# An empty node (a fresh fleet, PROD AU before any client) started with
# marker 0 while the snapshot it had just LOADED covered the WAL's old
# records; once its restarts had gone round the ring the oldest segment
# "still held seqs 1..1, NEWER than the last completed snapshot marker 0"
# and the node went FAILED - refusing writes until restarted, and every
# restart did it again.  PROD AU .65, the 0.5.6 roll: generation 9 of 8.
#
# One node, a 4-segment WAL: a key written and deleted (so the WAL holds
# seqs and the node holds nothing), then six clean restarts - each a stop
# snapshot and a new generation; the fifth start reuses the first
# segment.  Every start must log no "FULL at startup", come back READY and
# take a write.  FAIL-FIRST: before S334 the fifth start is FAILED.
# Usage: test/emptywraptest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcewr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18561
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "emptywraptest: port $PORT already bound" >&2; exit 1; }
mkdir -p "$D/wal"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = ewr-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 1
segments = 4
save = off
CONF
chmod 600 "$D/n.conf"
start() {
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && return 0; kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	echo "did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"; return 1
}
stop() { kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
# op <set|del|state> <key>: one request, prints the answer's gist
op() { python3 -c '
import json, pcnative, socket, sys, time
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10))
def call(m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
if sys.argv[2] == "state":
    for _ in range(50):
        s = call("stats")["result"].get("state")
        if s not in ("starting", "recovering"):
            break
        time.sleep(0.1)
    print(s)
elif sys.argv[2] == "set":
    print((call("set", col="c", key=sys.argv[3], value="v").get("result") or {}).get("stored"))
else:
    print(json.dumps(call("del", col="c", key=sys.argv[3]).get("result")))' $PORT "$@" 2>&1; }

start || { echo "emptywraptest: the node did not start"; exit 1; }
A=$(op set k1); op del k1 > /dev/null
[ "$A" = True ] && ok "a key written and deleted: the WAL holds records, the node holds nothing" \
	|| bad "the first write: $A"
r=1; first_bad=
while [ $r -le 6 ]; do
	stop
	start || { bad "restart $r: the node did not start"; break; }
	S=$(op state); W=$(op set "w$r"); op del "w$r" > /dev/null
	G=$(sed -n 's/.*wal: active in .*generation \([0-9]*\).*/\1/p' "$D/n.log" | tail -1)
	if grep -q "FULL at startup" "$D/n.log" || [ "$S" != ready ] || [ "$W" != True ]; then
		[ -z "$first_bad" ] && first_bad="restart $r (generation $G): state $S, write $W - $(grep -m1 'FULL at startup' "$D/n.log" | sed 's/.*CRITICAL: //' | cut -c1-110)"
	fi
	r=$((r + 1))
done
[ -z "$first_bad" ] && ok "six clean restarts, round the 4-segment ring and past it: READY every time, no FULL at startup, writes taken (last generation $G)" \
	|| bad "an empty node FAILED by its own restarts: $first_bad"
stop
echo "emptywraptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
