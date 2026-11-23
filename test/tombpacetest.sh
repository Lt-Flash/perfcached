#!/bin/sh
# tombpacetest.sh - a peer back from a long silence is caught up on the retained deletes and
# expiries, and the replay ENDS: a catch-up that falls behind the planting rate replays each one
# again as it nears the horizon, and the peer counts every replayed expiry as noted.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pctp.XXXXXX)
LP=
trap 'kill -CONT $(cat "$D"/n*.pid 2>/dev/null) 2>/dev/null; kill -9 $(cat "$D"/n*.pid 2>/dev/null) $LP 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 20461 20462 20463 20471 20472 20473 20480; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "tombpacetest: port $p busy" >&2; exit 1; }
done
# the catch-up's per-tick bound, lowered so a test's write rate shows what PROD's does
export PERFCACHED_TEST_TOMB_BURST=400

for n in 1 2 3; do
	mkdir -p "$D/wal$n" "$D/st$n"
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/st$n
[memory]
arena_mb = 64
[secrets]
client = tp-client-secret
cluster = tp-cluster-secret
[listen]
tcp = 127.0.0.1:2046$n
http = 127.0.0.1:2047$n
plaintext = loopback
[cluster]
multicast = 239.255.77.137:20480
advertise = 127.0.37.$n
mode = eager
collections = 0
tombstone_acks = no
tombstone_retain_s = 300
[collection 0]
buckets_log2 = 13
[wal]
dir = $D/wal$n
probe = no
segment_mb = 8
segments = 4
C
	chmod 600 "$D/n$n.conf"
	"$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
done
st() { curl -s "http://127.0.0.1:2047$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
def g(v): return v.get("total", v) if isinstance(v, dict) else v
c = [x for x in r["collections"] if x["name"] == "0"][0]; t = r["cluster"]["tombstones_retained"]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
up() { [ "$(st $1 'r["state"]')" = ready ] && [ "$(st $1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 2 ]; }
i=0; while [ $i -lt 120 ] && ! { up 1 && up 2 && up 3; }; do sleep 0.5; i=$((i+1)); done
up 1 && up 2 && up 3 && ok "three eager nodes formed" || { bad "the fleet did not form"; exit 1; }

# ~300 short-lived keys a second through nodes 2 and 3, each expiring 2 s on: every node notes
# each expiry once, by its own sweep
cat > "$D/load.py" <<'PY'
import random, socket, sys, time
fs = [socket.create_connection(("127.0.0.1", p), timeout=10).makefile("rwb") for p in (20462, 20463)]
n = 0
while True:
    t0 = time.time()
    for f in fs:
        for _ in range(15):
            k = "k%06d" % random.randrange(200000); a = ["SET", k, "v", "EX", "2"]
            f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
        f.flush()
        for _ in range(15):
            f.readline()
    time.sleep(max(0, 0.1 - (time.time() - t0)))
PY
python3 "$D/load.py" > "$D/load.log" 2>&1 & LP=$!
sleep 15
N1=$(st 1 'r["cluster"]["node"]')
# node 1 silent 105 s: past what the catch-up's first two windows reach, so the replay is paced
kill -STOP "$(cat "$D/n1.pid")"
sleep 105
L2=$(wc -l < "$D/n2.log"); L3=$(wc -l < "$D/n3.log")
L1=$(wc -l < "$D/n1.log")
kill -CONT "$(cat "$D/n1.pid")"
# back once its own loop has run since the pause (a stale master only then steps down and is
# CONNECTING a second or two) and up on two checks 2 s apart
i=0; while [ $i -lt 60 ] && ! { tail -n +$((L1 + 1)) "$D/n1.log" | grep -q " ms late" && up 1 && sleep 2 && up 1; }; do
	sleep 0.5; i=$((i+1))
done
[ $i -lt 60 ] && ok "node 1 is back in the fleet" || bad "node 1 did not come back"
# the replay must end well inside the time the fleet took to plant what it replays
sleep 60
kill -0 $LP 2>/dev/null && ok "the load runs ($(st 2 'g(c["expired"])') expired on node 2)" || bad "the load stopped: $(tail -2 "$D/load.log")"
for n in 2 3; do
	eval "l=\$L$n"
	tail -n +$((l + 1)) "$D/n$n.log" | grep -q "node $N1 caught up" \
		&& ok "node $n finished node 1's catch-up: $(tail -n +$((l + 1)) "$D/n$n.log" | grep "node $N1 caught up" | tail -1 | sed 's/.*caught up - //')" \
		|| bad "node $n is still replaying to node 1 60 s after its silence ended (replayed $(st $n 't["replayed"]') in all)"
done
# from here every node notes about one expiry per key it reaps itself
E0=$(st 1 'g(c["expired"])'); X0=$(st 1 'g(t["noted_expiries"])')
sleep 30
E1=$(st 1 'g(c["expired"])'); X1=$(st 1 'g(t["noted_expiries"])')
E=$((E1 - E0)); X=$((X1 - X0))
[ "$E" -gt 1000 ] && [ "$X" -le $((E + E / 10 + 200)) ] \
	&& ok "node 1 over 30 s: $X expiries noted for $E keys expired" \
	|| bad "node 1 over 30 s: $X expiries noted for $E keys expired (want <= $((E + E / 10 + 200)), and over 1000 expired)"

echo "tombpacetest: $pass passed, $fail failed"
[ "$fail" = 0 ]
