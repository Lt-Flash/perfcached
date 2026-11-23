#!/bin/sh
# legdraintest.sh - records a burst puts past their buckets' slots (into the chains) come back to
# their buckets as the table widens: a small table, a load faster than growth, every value intact.
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcleg.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

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

: > "$D/a.log"
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
(ok if leg0 > 1000 else bad)("the burst outran growth and filled the chains (%d records)" % leg0)

# growth runs on the maintenance tick: past leg_stuck_pct of entries chained, the table
# widens down to grow_floor_pct; the splitter is time-sliced, so the deadline bounds speed
DEADLINE = 15
end = time.time() + DEADLINE
while time.time() < end:
    c = col()
    if c.get("overflow", 0) * 100 < 8 * N and (c.get("leg_splits") or 0) > 0:
        break
    time.sleep(0.5)
settle = time.time() - (end - DEADLINE)
c = col()
leg1 = c.get("overflow", 0)
# under 8% of the entries chained: a chained bucket keeps 5 at home (slot 5 is the link),
# and between doublings the unsplit half (Poisson 4.9) leaves ~6% chained
(ok if leg1 * 100 < 8 * N else bad)(
    "the chains fell to near nothing: %d -> %d records, %.1f%% of the keyspace "
    "(%s buckets, %s splits asked for by the chains)"
    % (leg0, leg1, leg1 * 100.0 / N, c.get("buckets"), c.get("leg_splits")))
(ok if settle < DEADLINE else bad)(
    "and it got there in %.1f s, not a minute of slow lookups (deadline %d s)"
    % (settle, DEADLINE))
(ok if (c.get("leg_splits") or 0) > 0 else bad)(
    "and the chains are what asked for the width (%s splits, %s still chained)"
    % (c.get("leg_splits"), c.get("leg_stuck")))

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
