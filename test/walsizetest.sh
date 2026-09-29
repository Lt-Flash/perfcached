#!/bin/sh
# walsizetest.sh - RV-13: a WAL too small for its snapshots says so.
#
# A snapshot frees only the WAL written before it began.  If one takes
# longer than the WHOLE ring takes to fill at the current write rate, no
# trigger - the quarter or S224's rate trigger - can start it early
# enough: the ring fills first, overruns, and the node heals (S229), again
# and again.  RV-13 measured it: a 64 MB/s snapshot writer overran with its
# rate trigger armed and firing at 4 free segments.  That is sizing, and
# nothing said so.
#
# Two runs of one load (a 3 MB table of 4 KB values, a ~3 s snapshot at
# rdb_mb_s = 1, then overwrites from two connections paced to ~5 MB/s):
#   1. a 4 x 1 MB ring - it fills in ~0.8 s: the node warns, once, naming
#      the snapshot's duration and the ring's fill time, and
#      perfcached_wal_ring_fill_seconds reads below the snapshot duration;
#   2. a 64 x 1 MB ring - ~13 s to fill: no warning.
# FAIL-FIRST: the build before RV-13 has neither the warning nor the gauge.
# Usage: test/walsizetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcws.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18498 HPORT=18499
if ss -ltn 2>/dev/null | grep -qE ":1849[89][[:space:]]"; then
	echo "walsizetest: port $PORT/$HPORT already bound" >&2; exit 1
fi
run() { # run <segments> <label>
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 256
[secrets]
client = ws-client
[listen]
tcp = 127.0.0.1:$PORT
http = 127.0.0.1:$HPORT
plaintext = loopback
[collection c]
buckets_log2 = 14
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 1
segments = $1
ring_kb = 16384
save = off
rdb_mb_s = 1
CONF
	chmod 600 "$D/n.conf"
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
	python3 - "$PORT" "$HPORT" <<'PY' > "$D/$2.out" 2>&1
import json, socket, sys, time, urllib.request
port, hport = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port), timeout=30); f = s.makefile("rb")
def call(m, **p):
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode())
    return json.loads(f.readline())
def st(): return call("stats")["result"]
v = "x" * 4000                     # 4 KB: MB/s from a paced client
def batch(lo, n):
    s.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "k%05d" % (i % 750), "value": v}}) + "\n" for i in range(lo, lo + n)).encode())
    for _ in range(n): f.readline()
for lo in range(0, 750, 25): batch(lo, 25)
call("save")
for _ in range(200):
    r = st()["rdb"]
    if r["saves"] >= 1 and not r["running"]: break
    time.sleep(0.1)
print("DUR_MS=%d" % st()["rdb"]["last_dur_ms"])
# overwrites from TWO connections for 12 s, the lowest whole-ring fill
# time seen.  One connection reached ~2 MB/s - the ring at 2.6 s against a
# 3.1 s snapshot, too thin a margin for a slower runner; a 6 MB table for a
# longer snapshot overran the 4 MB ring during the prefill instead, and
# its heal's unthrottled snapshot (5 ms) is not a planning figure.
import threading
low = [None]; stop = [False]
def writer(k):
    ws = socket.create_connection(("127.0.0.1", port), timeout=30); wf = ws.makefile("rb")
    i = 750 + k
    while not stop[0]:
        tick = time.time()
        ws.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": j, "method": "set",
            "params": {"col": "c", "key": "k%05d" % (j % 750), "value": v}}) + "\n" for j in range(i, i + 120, 2)).encode())
        for _ in range(60): wf.readline()
        i += 120
        # PACED: ~2.4 MB/s each, ~5 MB/s together, so each arm clears the
        # snapshot's 3.1 s by ~4x - unpaced, two writers filled even the
        # control ring in under 3 s, and the warning there was right
        rest = 0.1 - (time.time() - tick)
        if rest > 0: time.sleep(rest)
ts = [threading.Thread(target=writer, args=(k,)) for k in (0, 1)]
[t.start() for t in ts]
t0 = time.time()
while time.time() - t0 < 12:
    rf = st()["wal"].get("ring_fill_s")
    if rf is not None and rf >= 0 and (low[0] is None or rf < low[0]): low[0] = rf
    time.sleep(0.1)
stop[0] = True; [t.join() for t in ts]
low = low[0]
print("RING_FILL_LOW=%s" % low)
m = urllib.request.urlopen("http://127.0.0.1:%d/metrics" % hport, timeout=5).read().decode()
print("METRIC=%s" % any(l.startswith("perfcached_wal_ring_fill_seconds ") for l in m.splitlines()))
PY
	kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
	cp "$D/n.log" "$D/$2.log"
}
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }

echo "--- 1. a 4 MB ring against a ~3 s snapshot at ~5 MB/s"
run 4 small
W=$(grep -c "snapshots cannot keep ahead of the WAL" "$D/small.log")
DUR=$(val small DUR_MS); LOW=$(val small RING_FILL_LOW)
[ "$W" = 1 ] && ok "the node warned, once: $(grep -m1 -o 'the last took [0-9.]* s, and the whole ring ([0-9]* segments) fills in [0-9.]* s' "$D/small.log")" \
	|| bad "the sizing warning appeared $W time(s) (want 1); snapshot $DUR ms"
python3 -c "import sys; sys.exit(0 if $LOW is not None and 0 <= $LOW < $DUR / 1000 else 1)" 2>/dev/null \
	&& ok "and ring_fill_s read $LOW s, under the snapshot's $DUR ms - an alert can compare the two" \
	|| bad "ring_fill_s lowest $LOW against a $DUR ms snapshot"
[ "$(val small METRIC)" = True ] && ok "perfcached_wal_ring_fill_seconds is on /metrics" \
	|| bad "perfcached_wal_ring_fill_seconds missing from /metrics"

echo "--- 2. the same load, a 64 MB ring"
run 64 big
W2=$(grep -c "snapshots cannot keep ahead of the WAL" "$D/big.log")
[ "$W2" = 0 ] && ok "no warning when the ring outlasts the snapshot (ring fill low $(val big RING_FILL_LOW) s, snapshot $(val big DUR_MS) ms)" \
	|| bad "warned $W2 time(s) with a ring that outlasts the snapshot"

echo "walsizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
