#!/bin/sh
# legdraintest.sh - S172: records that land in the overflow leg get put
# back into the table.
#
# A key lives in its bucket OR in the leg.  The leg is a chain per hash
# under ONE lock, so a lookup that misses the bucket walks it - and
# until this, nothing ever moved a record back: splits make room in the
# buckets and never look at the leg (S134).
#
# Measured on the test fleet 2026-09-20: loading 62,000 keys at 86,000/s
# into a 4,096-bucket collection left 39,390 records (62%) in the leg
# PERMANENTLY.  Table growth runs on the 1 Hz maintenance thread in
# bounded chunks, so a sub-second bulk load outruns it completely; the
# table then grew to its target load factor and stopped, leg and all.
# KEYS took 15.7 ms and GET 44 us in that state.
#
# The test reproduces exactly that: a deliberately small table, a burst
# far faster than growth, then the property - the leg drains and every
# record is still readable, with its value intact.
# Usage: test/legdraintest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcleg.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT

cat > "$D/a.conf" <<EOF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = leg-client-secret
[listen]
resp = 127.0.0.1:17684
http = 127.0.0.1:17685
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
chmod 600 "$D/a.conf"

"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PYEOF'
import json, socket, sys, time, urllib.request

N = 40000
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def col():
    s = json.loads(urllib.request.urlopen("http://127.0.0.1:17685/stats", timeout=10).read())
    for c in s["collections"]:
        if c["name"] == "0":
            return c
    return {}

s = socket.create_connection(("127.0.0.1", 17684), 8); s.settimeout(60)
f = s.makefile("rwb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out
def parse():
    h = f.readline(); t, b = h[:1], h[1:].rstrip(b"\r\n")
    if t == b":": return int(b)
    if t == b"+": return b
    if t == b"-": return Exception(b.decode())
    if t == b"$":
        n = int(b); return None if n < 0 else f.read(n+2)[:-2]
    raise ValueError(h)

# the burst: far faster than a 1 Hz grower can follow
t0 = time.time()
for base in range(0, N, 1000):
    s.sendall(b"".join(cmd("SET", b"leg:%d" % (base+j), b"v%d" % (base+j)) for j in range(1000)))
    f.flush()
    for _ in range(1000):
        parse()
dt = time.time() - t0
c = col()
leg0 = c.get("overflow", 0)
print("  ..   loaded %d in %.2f s (%.0f/s); table %s buckets, leg %s" %
      (N, dt, N/dt, c.get("buckets"), leg0))
(ok if leg0 > 1000 else bad)("the burst outran growth and filled the leg (%d records)" % leg0)

# The drain runs on the maintenance tick; give it up to 60 s and stop
# when it stops making progress.
#
# S172 alone could not empty the leg, and the arithmetic says why: at
# the default 75% target with 6 slots a bucket, Poisson(4.5) leaves
# ~30% of buckets full and ~7% of RECORDS in the leg with nowhere to
# return to - measured, 3,390 of 40,000.  S175 makes that residue the
# signal: a drain pass that cannot re-home more than leg_stuck_pct of
# the entries widens the table past grow_at_pct, down to
# grow_floor_pct (50), where the same arithmetic leaves ~1.7%.
# S176: the drain is bounded by a TIME SLICE and a whole pass, not by a
# flat count, so this is also an assertion about SPEED.  With the old
# 1,024-a-tick budget the leg came back at ~630 records/s and the table
# did not widen until a whole pass had finished - fifty seconds on the
# fleet.  With the slice: 2 s, measured.  Fifteen is the deadline, which
# the old budget cannot meet from 15,912 records however lucky it gets.
DEADLINE = 15
end = time.time() + DEADLINE
while time.time() < end:
    c = col()
    if c.get("overflow", 0) * 100 < 5 * N and (c.get("leg_splits") or 0) > 0:
        break
    time.sleep(0.5)
settle = time.time() - (end - DEADLINE)
c = col()
leg1 = c.get("overflow", 0)
# 5% of the entries.  The 75% geometry leaves ~7% by the arithmetic and
# 8.5% measured; the 50% floor measures 3.9%, not the 1.7% a uniform
# Poisson(3) implies, because linear hashing splits buckets IN ORDER -
# between two power-of-two levels the table holds split buckets at half
# the load of the unsplit ones, and the fuller half is where the leg
# comes from.  The uniform figure only arrives at a doubling.
(ok if leg1 * 100 < 5 * N else bad)(
    "the leg fell to near nothing: %d -> %d records, %.1f%% of the keyspace "
    "(%s moved back, %s buckets, %s splits asked for by the leg)"
    % (leg0, leg1, leg1 * 100.0 / N, c.get("leg_drained"), c.get("buckets"),
       c.get("leg_splits")))
(ok if settle < DEADLINE else bad)(
    "and it got there in %.1f s, not a minute of slow lookups (deadline %d s)"
    % (settle, DEADLINE))
(ok if (c.get("leg_splits") or 0) > 0 else bad)(
    "and the leg is what asked for the width (%s splits, %s stuck in the last pass)"
    % (c.get("leg_splits"), c.get("leg_stuck")))
(ok if (c.get("leg_drained") or 0) >= leg0 - leg1 else bad)(
    "the drain accounts for the difference (moved %s >= %d)" % (c.get("leg_drained"), leg0 - leg1))

# and the records are all still there, with their values
miss = wrong = 0
for i in range(0, N, 7):
    s.sendall(cmd("GET", b"leg:%d" % i)); f.flush()
    v = parse()
    if v is None: miss += 1
    elif v != b"v%d" % i: wrong += 1
(ok if miss == 0 and wrong == 0 else bad)(
    "every sampled record survived the move (%d missing, %d wrong)" % (miss, wrong))
(ok if col().get("entries") == N else bad)("the table still holds all %d entries (%s)" % (N, col().get("entries")))

print("legdraintest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PYEOF
rc=$?
exit $rc
