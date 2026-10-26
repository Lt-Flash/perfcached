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
# S437: /stats and /metrics give the ring's shape (4 segments of 1 MB =
# 4 MB) and its use, and the wal.capacity alert is raised in run 1 (the
# whole ring fills faster than one snapshot takes) and not in run 2 - its
# minute's hold is 3 s here (PERFCACHED_ALERT_FAST=20).
# FAIL-FIRST: the build before RV-13 has neither the warning nor the gauge;
# the build before S437 has no segments/size_mb, no gauges and no alert.
# Usage: test/walsizetest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
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
export PERFCACHED_ALERT_FAST=20        # S437: wal.capacity's minute -> 3 s
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
import json, pcnative, socket, sys, time, urllib.request
port, hport = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port), timeout=30); f = pcnative.wrap(s)
def call(m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def st(): return call("stats")["result"]
v = "x" * 4000                     # 4 KB: MB/s from a paced client
def batch(lo, n):
    f.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "k%05d" % (i % 750), "value": v}}) + "\n" for i in range(lo, lo + n)).encode()); f.flush()
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
    ws = socket.create_connection(("127.0.0.1", port), timeout=30); wf = pcnative.wrap(ws)
    i = 750 + k
    while not stop[0]:
        tick = time.time()
        wf.write("".join(json.dumps({"jsonrpc": "2.0", "id": j, "method": "set",
            "params": {"col": "c", "key": "k%05d" % (j % 750), "value": v}}) + "\n" for j in range(i, i + 120, 2)).encode()); wf.flush()
        for _ in range(60): wf.readline()
        i += 120
        # PACED: ~2.4 MB/s each, ~5 MB/s together, so each arm clears the
        # snapshot's 3.1 s by ~4x - unpaced, two writers filled even the
        # control ring in under 3 s, and the warning there was right
        rest = 0.1 - (time.time() - tick)
        if rest > 0: time.sleep(rest)
ts = [threading.Thread(target=writer, args=(k,)) for k in (0, 1)]
[t.start() for t in ts]
t0 = time.time(); alert = [None]
while time.time() - t0 < 12:
    sn = st()
    rf = sn["wal"].get("ring_fill_s")
    if rf is not None and rf >= 0 and (low[0] is None or rf < low[0]): low[0] = rf
    for a in (sn.get("alerts") or {}).get("active") or []:
        if a.get("key") == "wal.capacity" and alert[0] is None:
            alert[0] = "%s: %s" % (a.get("level"), a.get("text"))
    time.sleep(0.1)
stop[0] = True; [t.join() for t in ts]
low = low[0]
print("RING_FILL_LOW=%s" % low)
m = urllib.request.urlopen("http://127.0.0.1:%d/metrics" % hport, timeout=5).read().decode()
print("METRIC=%s" % any(l.startswith("perfcached_wal_ring_fill_seconds ") for l in m.splitlines()))
# S437
w = st()["wal"]
print("SHAPE=%s/%s/%s/%s" % (w.get("segments"), w.get("segment_mb"), w.get("size_mb"),
    "pct" if isinstance(w.get("used_pct"), int) and 0 <= w["used_pct"] <= 100 else w.get("used_pct")))
g = dict(l.split(" ", 1) for l in m.splitlines() if l.startswith("perfcached_wal_") and " " in l)
print("GAUGES=%s/%s/%s" % (g.get("perfcached_wal_segments"), g.get("perfcached_wal_size_bytes"),
    "ratio" if g.get("perfcached_wal_used_ratio") is not None else None))
print("ALERT=%s" % alert[0])
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
[ "$(val small SHAPE)" = "4/1/4/pct" ] && ok "S437: /stats says 4 segments of 1 MB, 4 MB, and the share in use" \
	|| bad "S437: /stats wal segments/segment_mb/size_mb/used_pct: $(val small SHAPE) (want 4/1/4/pct)"
[ "$(val small GAUGES)" = "4/4194304/ratio" ] && ok "S437: /metrics has the segments, the size in bytes and the used ratio" \
	|| bad "S437: /metrics perfcached_wal_segments/size_bytes/used_ratio: $(val small GAUGES)"
# either of its two comparisons raises it, and its text is the first's:
# early in the load the rate is still low and the free segments ran out
# first (GitHub 39952740047), later the whole ring fills faster too
case "$(val small ALERT)" in "warning: the whole WAL (4 x 1 MB) fills in "*|"warning: the WAL's "*" free segment(s) of 4 (1 MB each) last "*) \
	ok "S437: the wal.capacity alert was raised - $(val small ALERT | cut -c1-140)";;
	*) bad "S437: no wal.capacity alert while the ring filled faster than a snapshot: $(val small ALERT)";; esac

echo "--- 2. the same load, a 64 MB ring"
run 64 big
W2=$(grep -c "snapshots cannot keep ahead of the WAL" "$D/big.log")
[ "$(val big ALERT)" = None ] && ok "S437: and no wal.capacity alert" \
	|| bad "S437: wal.capacity raised on the 64 MB ring: $(val big ALERT)"
[ "$W2" = 0 ] && ok "no warning when the ring outlasts the snapshot (ring fill low $(val big RING_FILL_LOW) s, snapshot $(val big DUR_MS) ms)" \
	|| bad "warned $W2 time(s) with a ring that outlasts the snapshot"

echo "walsizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
