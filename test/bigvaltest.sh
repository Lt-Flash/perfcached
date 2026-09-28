#!/bin/sh
# bigvaltest.sh - S174: values up to the largest cell a chunk can cut.
#
# The arena's top size class was 64 KB, so a value over ~65,470 bytes was
# refused.  Measured against the operator's production keyspace (61,944
# keys, 2026-09-20): 19 values did not fit, the largest 218,265 bytes -
# so the ceiling was a product limit, not a theoretical one.
#
# A chunk is ONE 256 KB slot with a 64-byte header, so the largest class
# that can be cut is 262,080 (exactly one cell, no waste); 262,144 would
# cut zero cells.  That is the ceiling this pins, at the doors and
# through the walk - a record larger than the walk's snapshot buffers
# would be truncated on KEYS/dump/replication rather than refused.
# Usage: test/bigvaltest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcbv.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[wal]
dir = $D/wal
probe = no
fsync = everysec
ring_kb = 1024
segment_mb = 4
segments = 4
[secrets]
client = bv-client-secret
[listen]
resp = 127.0.0.1:17688
http = 127.0.0.1:17689
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
mkdir -p "$D/wal"

"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PYEOF'
import json, socket, sys, urllib.request

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17688), 8); s.settimeout(60)
f = s.makefile("rwb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out
def parse():
    h = f.readline()
    t, b = h[:1], h[1:].rstrip(b"\r\n")
    if t == b"+": return b
    if t == b"-": return Exception(b.decode())
    if t == b":": return int(b)
    if t == b"$":
        n = int(b); return None if n < 0 else f.read(n+2)[:-2]
    raise ValueError(h)
def call(*a):
    s.sendall(cmd(*a)); f.flush(); return parse()

# a value of n bytes that cannot be mistaken for another: a fixed
# pattern with its own length woven through it
def mkval(n):
    seed = ("%d-" % n).encode()
    v = (seed * (n // len(seed) + 1))[:n]
    return v[:n-1] + b"Z"

# 213.1 kB is the largest value in the operator's production keyspace;
# 98,304 and 131,072 are class boundaries; 65,470 is the OLD ceiling.
for n in (65470, 98304, 131072, 218265):
    v = mkval(n)
    r = call("SET", "big:%d" % n, v)
    if isinstance(r, Exception):
        bad("SET of %d bytes was refused: %s" % (n, r)); continue
    got = call("GET", "big:%d" % n)
    if isinstance(got, Exception):
        bad("%d bytes stored but GET failed: %s" % (n, got))
    elif got is None:
        bad("%d bytes stored but GET missed" % n)
    elif got != v:
        bad("%d bytes came back changed (%d bytes, %s)" %
            (n, len(got), "prefix differs" if got[:32] != v[:32] else "tail differs"))
    else:
        ok("a %6d-byte value stores and reads back byte for byte" % n)

# the ceiling itself: one byte past the largest record a chunk can hold
# must be REFUSED, not truncated
CELL_MAX = 262080
over = CELL_MAX          # + header + key: cannot fit
r = call("SET", "big:over", mkval(over))
(ok if isinstance(r, Exception) else bad)(
    "a value past the ceiling is refused, not truncated (%s)" %
    (r if isinstance(r, Exception) else "STORED"))
(ok if call("GET", "big:over") is None else bad)("and nothing was stored for it")

# the walk must carry the big records whole: scan with values, and the
# keys enumeration must see them
s.sendall(cmd("SCAN", "0", "COUNT", "1000")); f.flush()
h = f.readline()
seen = 0
if h[:1] == b"*":
    f.readline(); f.readline()          # cursor bulk
    hh = f.readline()
    if hh[:1] == b"*":
        for _ in range(int(hh[1:])):
            hk = f.readline()
            k = f.read(int(hk[1:])+2)[:-2]
            if k.startswith(b"big:"):
                seen += 1
(ok if seen >= 4 else bad)("the keys walk offers every big record (%d of 4)" % seen)

vals = {}
for n in (65470, 98304, 131072, 218265):
    got = call("GET", "big:%d" % n)
    vals[n] = isinstance(got, bytes) and len(got) == n
(ok if all(vals.values()) else bad)(
    "every big record survives a walk over the table (%s)" %
    ",".join("%d:%s" % (n, "ok" if v else "BAD") for n, v in vals.items()))

st = json.loads(urllib.request.urlopen("http://127.0.0.1:17689/stats", timeout=10).read())
mem = st["memory"]
print("  ..   arena: %s live, tier %s" % (mem.get("live_mb", mem.get("live")), mem.get("tier")))

print("bigvaltest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PYEOF
rc=$?
[ $rc = 0 ] || exit $rc

# ---- durability: a record the store accepts must reach the log --------
# The WAL kept a record cap of its own, a flat 80 KB - above the old cell
# ceiling and below the new one.  A big value would have been acknowledged
# to the client and then dropped from the log, and would vanish on the
# next restart.
kill -TERM $P1 2>/dev/null; wait $P1 2>/dev/null; P1=
: > "$D/a.log"                 # the ready line must be THIS start's
"$BIN" -f "$D/a.conf" >> "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 300 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "  FAIL the node did not come back"; tail -3 "$D/a.log"; exit 1; }

python3 - <<PYEOF
import socket, sys

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17688), 8); s.settimeout(60)
f = s.makefile("rwb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out
def parse():
    h = f.readline()
    t, b = h[:1], h[1:].rstrip(b"\r\n")
    if t == b"+": return b
    if t == b"-": return Exception(b.decode())
    if t == b":": return int(b)
    if t == b"$":
        n = int(b); return None if n < 0 else f.read(n+2)[:-2]
    raise ValueError(h)
def mkval(n):
    seed = ("%d-" % n).encode()
    v = (seed * (n // len(seed) + 1))[:n]
    return v[:n-1] + b"Z"

back = {}
for n in (65470, 98304, 131072, 218265):
    s.sendall(cmd("GET", "big:%d" % n)); f.flush()
    got = parse()
    back[n] = isinstance(got, bytes) and got == mkval(n)
(ok if all(back.values()) else bad)(
    "every big value came back after a restart (%s)" %
    ",".join("%d:%s" % (n, "ok" if v else "LOST") for n, v in back.items()))

print("bigvaltest durability: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PYEOF
rc=$?
exit $rc
