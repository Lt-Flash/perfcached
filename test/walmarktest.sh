#!/bin/sh
# walmarktest.sh - the WAL's synced watermark: it does not pass a number a writer holds but has not
# yet put in its ring, and a dropped number does not hold a sync.  Usage: test/walmarktest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwm.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
ss -ltn 2>/dev/null | grep -q ":16488[[:space:]]" && { echo "walmarktest: port 16488 busy" >&2; exit 1; }
mkdir -p "$D/wal"
cat > "$D/w.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = wm-client-secret
cluster = wm-cluster-secret
[listen]
tcp = 127.0.0.1:16488
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 4
segments = 4
save = off
[collection c]
buckets_log2 = 10
EOF
chmod 600 "$D/w.conf"
# a writer of a "stall*" key sleeps 3 s between taking its sequence number and its ring; a "drop*"
# key's record is dropped as a full ring drops one
: > "$D/log"
PERFCACHED_TEST_WAL_STALL_MS=3000 PERFCACHED_TEST_WAL_DROP=1 "$BIN" -f "$D/w.conf" > "$D/log" 2>&1 &
PID=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/log" || { echo "walmarktest: the daemon did not start"; tail -5 "$D/log"; exit 1; }

python3 - <<'EOF'
import json, pcnative, select, socket, sys, time
def conn():
    return pcnative.wrap(socket.create_connection(("127.0.0.1", 16488), timeout=20))
rid = [0]
def send(f, m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
def call(f, m, **p):
    send(f, m, **p)
    return json.loads(f.readline())
fails = 0
def ok(m): print("  ok   " + m)
def bad(m):
    global fails
    fails += 1
    print("  FAIL " + m)

# 1. writer A holds number N, unwritten, for 3 s; writer B on the other worker logs N+1 and asks
#    for a sync.  The answer may not say N+1 is durable while N is nowhere on the device
a = conn()
cands = [conn() for _ in range(12)]
send(a, "set", col="c", key="stall1", value="a")
time.sleep(0.3)
for f in cands: send(f, "ping")
time.sleep(0.5)
other = [f for f in cands if select.select([f], [], [], 0)[0]]
if not other:
    bad("1: no connection reached the other worker")
    sys.exit(1)
b = other[0]
b.readline()
call(b, "set", col="c", key="b1", value="b")
t0 = time.time()
r = call(b, "sync")
took = time.time() - t0
a_done = bool(select.select([a], [], [], 0.5)[0])
if "result" not in r:
    bad("1: sync failed: %r" % r)
elif not a_done:
    bad("1: sync answered in %.0f ms with seq %s synced while the stalled write, numbered below "
        "it, was still not in its ring" % (took * 1000, r["result"].get("seq")))
else:
    ok("1: sync waited for the stalled write's record (%.0f ms)" % (took * 1000))

# 2. the newest number goes to a record a full ring dropped: everything appended is still synced
#    at once, and the answer owns the drop
c = conn()
call(c, "set", col="c", key="x2", value="x")
call(c, "set", col="c", key="drop2", value="x")
t0 = time.time()
r = call(c, "sync", timeout_ms=3000)
took = time.time() - t0
if "result" not in r:
    bad("2: sync after the newest record was dropped answered %r in %.0f ms" % (r, took * 1000))
elif r["result"].get("dropped", 0) < 1:
    bad("2: sync after a dropped record does not report it: %r" % r["result"])
else:
    ok("2: sync after a dropped record answered in %.0f ms, dropped %d"
       % (took * 1000, r["result"]["dropped"]))
sys.exit(1 if fails else 0)
EOF
rc=$?
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
if [ $rc -ne 0 ]; then
	echo "walmarktest: FAILED"; grep -iE "wal|error" "$D/log" | tail -5
	exit 1
fi
echo "walmarktest: passed"
