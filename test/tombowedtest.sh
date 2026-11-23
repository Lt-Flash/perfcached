#!/bin/sh
# tombowedtest.sh - a peer that goes silent while a catch-up replay is still owed to it is replayed
# to ONCE on its return: the owed replay already covers the silence, so the silence noted with it
# must join that replay, not start a second one over the same window once the first one ends.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcto.XXXXXX)
LP=
trap 'kill -CONT $(cat "$D"/n*.pid 2>/dev/null) 2>/dev/null; kill -9 $(cat "$D"/n*.pid 2>/dev/null) $LP 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 20661 20662 20663 20671 20672 20673 20680; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "tombowedtest: port $p busy" >&2; exit 1; }
done

# tombstone_retain_s 3600: a new member's first catch-up walks from the horizon in windows of
# 60, 120, 240 ... s, one a tick (10 s), so it is owed for ~50 s and node 1 goes silent inside it
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
client = to-client-secret
cluster = to-cluster-secret
[listen]
tcp = 127.0.0.1:2066$n
http = 127.0.0.1:2067$n
plaintext = loopback
[cluster]
multicast = 239.255.77.247:20680
advertise = 127.0.247.$n
mode = eager
collections = 0
tombstone_acks = no
tombstone_retain_s = 3600
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
st() { curl -s "http://127.0.0.1:2067$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
up() { [ "$(st $1 'r["state"]')" = ready ] && [ "$(st $1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 2 ]; }
i=0; while [ $i -lt 120 ] && ! { up 1 && up 2 && up 3; }; do sleep 0.5; i=$((i+1)); done
up 1 && up 2 && up 3 && ok "three eager nodes formed" || { bad "the fleet did not form"; exit 1; }
N1=$(st 1 'r["cluster"]["node"]')

# ~100 short-lived keys a second through nodes 2 and 3
cat > "$D/load.py" <<'PY'
import random, socket, time
fs = [socket.create_connection(("127.0.0.1", p), timeout=10).makefile("rwb") for p in (20662, 20663)]
while True:
    t0 = time.time()
    for f in fs:
        for _ in range(5):
            k = "k%06d" % random.randrange(200000); a = ["SET", k, "v", "EX", "2"]
            f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
        f.flush()
        for _ in range(5):
            f.readline()
    time.sleep(max(0, 0.1 - (time.time() - t0)))
PY
python3 "$D/load.py" > "$D/load.log" 2>&1 & LP=$!
sleep 15
L2=$(wc -l < "$D/n2.log"); L3=$(wc -l < "$D/n3.log")
early=$(grep -c "node $N1 caught up" "$D/n2.log" "$D/n3.log" | awk -F: '{s += $2} END {print s + 0}')
kill -STOP "$(cat "$D/n1.pid")"
sleep 30
L1=$(wc -l < "$D/n1.log")
kill -CONT "$(cat "$D/n1.pid")"
# back once its own loop has run since the pause (a stale master only then steps down and is
# CONNECTING a second or two) and up on two checks 2 s apart
i=0; while [ $i -lt 60 ] && ! { tail -n +$((L1 + 1)) "$D/n1.log" | grep -q " ms late" && up 1 && sleep 2 && up 1; }; do
	sleep 0.5; i=$((i+1))
done
[ $i -lt 60 ] && ok "node 1 is back in the fleet" || bad "node 1 did not come back"
# the owed replay ends within a few ticks; a second one over the silence would follow a tick later
sleep 60
kill -0 $LP 2>/dev/null && ok "the load runs" || bad "the load stopped: $(tail -2 "$D/load.log")"
[ "$early" = 0 ] && ok "node 1's first catch-up was still owed when it went silent" \
	|| bad "a peer finished node 1's first catch-up before its silence - nothing was owed"
for n in 2 3; do
	eval "l=\$L$n"
	c=$(tail -n +$((l + 1)) "$D/n$n.log" | grep -c "node $N1 caught up")
	[ "$c" = 1 ] && ok "node $n replayed to node 1 once after its silence: $(tail -n +$((l + 1)) "$D/n$n.log" | grep -o "node $N1 caught up.*")" \
		|| bad "node $n replayed to node 1 $c time(s) after its silence: $(tail -n +$((l + 1)) "$D/n$n.log" | grep -o "node $N1 caught up.*" | tr '\n' ';')"
done

echo "tombowedtest: $pass passed, $fail failed"
[ "$fail" = 0 ]
