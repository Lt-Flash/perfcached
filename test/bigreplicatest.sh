#!/bin/sh
# bigreplicatest.sh - S174: an eager collection replicates values that
# are too large for a datagram.
#
# PC_MAX_FWD_VAL is 58,000 - a UDP limit - and until 2026-09-20 both the
# write-path push and the sweep behind it simply SKIPPED anything above
# it (counted as migrate_skipped_big).  With the store's ceiling at
# 64 KB that was a 7.5 KB band nobody noticed; with it at 256 KB it
# would have meant a 218 KB value living on one node of three and
# reading as a miss on the other two.
#
# The TCP bulk plane already carried oversized records for placement
# moves.  This asserts that the eager sweep now uses it, that what
# arrives is a PASSIVE copy (a replica that re-propagated would echo the
# keyspace around the fleet), and that a small value's path is unchanged.
# Usage: test/bigreplicatest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcbr.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

node() { # node <n> <resp> <http>  (advertises 127.0.1.2<n>)
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 96
[secrets]
client = br-client-secret
cluster = br-cluster-secret
[listen]
resp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = 239.255.77.48:17148
advertise = 127.0.1.2$1
[collection 0]
buckets_log2 = 12
mode = eager
CONF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

node 1 17696 17697
node 2 17698 17699
start 1
start 2

python3 - <<'PY_EOF'
import json, socket, sys, time, urllib.request

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def conn(port):
    s = socket.create_connection(("127.0.0.1", port), 8); s.settimeout(60)
    return s, s.makefile("rwb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out
def parse(f):
    h = f.readline()
    t, b = h[:1], h[1:].rstrip(b"\r\n")
    if t == b"+": return b
    if t == b"-": return Exception(b.decode())
    if t == b":": return int(b)
    if t == b"$":
        n = int(b); return None if n < 0 else f.read(n+2)[:-2]
    raise ValueError(h)
def call(sf, *a):
    s, f = sf; s.sendall(cmd(*a)); f.flush(); return parse(f)
def stats(port):
    return json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d/stats" % port, timeout=10).read())
def mkval(n):
    seed = ("%d-" % n).encode()
    v = (seed * (n // len(seed) + 1))[:n]
    return v[:n-1] + b"Z"

N1, N2 = conn(17696), conn(17698)

# both nodes must see each other before anything is written, or the
# write-path push has nowhere to go and only the sweep is under test
end = time.time() + 30
while time.time() < end:
    if stats(17697)["cluster"].get("peers") and stats(17699)["cluster"].get("peers"):
        break
    time.sleep(0.5)
peers = len(stats(17697)["cluster"].get("peers") or [])
(ok if peers == 1 else bad)("the two nodes see each other (%d peer)" % peers)

# and both must be READY: a node still bootstrapping refuses writes
# outright ("LOADING"), which is not what this test is about
end = time.time() + 60
while time.time() < end:
    if stats(17697)["state"] == "ready" and stats(17699)["state"] == "ready":
        break
    time.sleep(0.5)
st = (stats(17697)["state"], stats(17699)["state"])
(ok if st == ("ready", "ready") else bad)("both nodes are ready %s" % (st,))

# a small value first: the datagram path, unchanged, and proof that the
# harness itself replicates
r = call(N1, "SET", "br:small", b"s" * 1000)
end = time.time() + 30
while time.time() < end and not isinstance(call(N2, "GET", "br:small"), bytes):
    time.sleep(0.5)
got = call(N2, "GET", "br:small")
(ok if got == b"s" * 1000 else bad)(
    "a 1,000-byte value reaches the other node (set=%s, got=%s, n1=%s n2=%s entries)"
    % (r, type(got).__name__,
       stats(17697)["collections"][0]["entries"],
       stats(17699)["collections"][0]["entries"]))

BIG = 218265                       # the largest value in production
v = mkval(BIG)
r = call(N1, "SET", "br:big", v)
(ok if not isinstance(r, Exception) else bad)("a %d-byte value is accepted" % BIG)

# the sweep runs every REPL_SWEEP_BEATS; give it several turns
t0 = time.time()
got = None
while time.time() - t0 < 60:
    got = call(N2, "GET", "br:big")
    if isinstance(got, bytes):
        break
    time.sleep(1)
if isinstance(got, bytes) and got == v:
    ok("it reaches the other node whole, in %.0f s (%d bytes)" % (time.time()-t0, len(got)))
elif isinstance(got, bytes):
    bad("it reached the other node CHANGED (%d of %d bytes)" % (len(got), BIG))
else:
    sk = stats(17697)["cluster"].get("migrate_skipped_big")
    bad("it never reached the other node in %.0f s (skipped_big=%s)" %
        (time.time()-t0, sk))

# the bulk plane carried it, not a datagram
b_in = stats(17699)["cluster"].get("bulk_in") or 0
(ok if b_in >= 1 else bad)("the copy arrived over the bulk plane (bulk_in=%d)" % b_in)

# and it arrived PASSIVE: a copy that re-propagated would come back to
# node 1 and be refused there as older, every sweep, for ever
o0 = stats(17697)["cluster"].get("recv_older") or 0
r0 = stats(17699)["cluster"].get("repl_out") or 0
time.sleep(25)
o1 = stats(17697)["cluster"].get("recv_older") or 0
r1 = stats(17699)["cluster"].get("repl_out") or 0
(ok if o1 == o0 else bad)(
    "the copy is passive: node 1 refused nothing as older over 25 s (%d -> %d)"
    % (o0, o1))
(ok if r1 == r0 else bad)(
    "and node 2 pushed nothing back (repl_out %d -> %d)" % (r0, r1))

lost = stats(17697)["cluster"].get("repl_bulk_lost") or 0
(ok if lost == 0 else bad)("no oversized batch went unconfirmed (%d)" % lost)

print("bigreplicatest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PY_EOF
rc=$?
exit $rc
