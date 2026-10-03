#!/bin/sh
# scanresizetest.sh - S257: a SCAN that spans a resize returns every key.
#
# The scan cursor was a bucket index into whichever table was live, on the
# premise that tables only grow in place (3.4).  Since S69 a resize is a
# migration and a swap: after a SHRINK a cursor past the new table's size
# walked nothing and reported the scan complete; after a GROW it indexed
# buckets holding other keys.  Found by the spread census: a node's walk
# outlasted its auto-shrink and counted 294k of its 641k keys.  (A GROW
# turned out safe for the old cursor - see the fail-first note below.)
# One node, collection 0 (the RESP door's) at 2^16 buckets, autoscale off,
# 20,000 keys:
#   1. shrink - the native door's `scan`, 256 buckets a page, paused after
#      40 pages (10,240 buckets: past the 8,192 the table shrinks to); resize
#      to 2^13 (20,000 keys refuse anything smaller) and wait until it has
#      swapped AND drained; finish the scan;
#   2. grow - the RESP door's SCAN, COUNT 64, paused after 8 pages; resize
#      back to 2^16, wait; finish;
#   each: every one of the 20,000 keys returned at least once.
# Fail-first: rc50 ends the shrink scan early - 3,271 of 20,000 keys, the
# scan reported complete.  The grow arm passes on rc50 too: the bucket index
# is the hash's low bits, so a key's bucket after a grow keeps its old low
# bits and an ascending cursor still meets every key it has not seen - it
# stays as the guard for the new cursor, not as a fail-first.
# Usage: test/scanresizetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcscanrz.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkdir -p "$D/s"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = scanrz-client-secret
enable = scanrz-enable-secret
[listen]
tcp = 127.0.0.1:17586
resp = 127.0.0.1:17587
http = 127.0.0.1:17588
plaintext = loopback
[collection 0]
buckets_log2 = 16
autoscale = off
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the daemon did not start: $(tail -1 "$D/n.log")"
	echo "scanresizetest: $pass passed, $fail failed"; exit 1
fi

RESFILE="$D/res" timeout 120 python3 - 2> "$D/py.err" <<'PY'
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
N = 20000
KEYS = {"k%05d" % i for i in range(N)}
j = socket.create_connection(("127.0.0.1", 17586), timeout=30); jf = j.makefile("rb")
def call(m, **p):
    j.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode())
    return json.loads(jf.readline())
def col():
    r = json.load(urllib.request.urlopen("http://127.0.0.1:17588/stats", timeout=5))
    return [c for c in r["collections"] if c["name"] == "0"][0]
def settled(buckets):
    for _ in range(200):
        c = col()
        if c.get("buckets") == buckets and not c.get("resizing_to"):
            return True
        time.sleep(0.1)
    return False
# fill
for b in range(0, N, 500):
    j.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "0", "key": "k%05d" % i, "value": "v"}}) + "\n" for i in range(b, min(N, b + 500))).encode())
    for _ in range(b, min(N, b + 500)):
        jf.readline()
check(col().get("entries") == N and col().get("buckets") == 65536,
      "20,000 keys in collection 0 at 65,536 buckets")
call("enable", secret="scanrz-enable-secret")

# 1. shrink under the native scan
seen, cur, pages = set(), 0, 0
while True:
    r = call("scan", col="0", cursor=cur, count=256)["result"]
    seen.update(it["k"] for it in r["items"])
    cur, pages = r["cursor"], pages + 1
    if not cur or pages == 40:
        break
mid = len(seen)
rz = call("resize", col="0", buckets_log2=13)
ok = settled(8192)
while cur:
    r = call("scan", col="0", cursor=cur, count=256)["result"]
    seen.update(it["k"] for it in r["items"])
    cur = r["cursor"]
    pages += 1
check(ok and "result" in rz, "shrink: paused at %d keys after 40 pages, resized to 8,192 buckets (swapped and drained)" % mid)
check(KEYS <= seen, "shrink: the scan returned %d of the 20,000 keys (every one at least once)" % len(KEYS & seen))

# 2. grow under the RESP SCAN
r = socket.create_connection(("127.0.0.1", 17587), timeout=30); rf = r.makefile("rb")
def scan_resp(cursor):
    r.sendall(b"*4\r\n$4\r\nSCAN\r\n$%d\r\n%s\r\n$5\r\nCOUNT\r\n$2\r\n64\r\n" % (len(str(cursor)), str(cursor).encode()))
    assert rf.readline() == b"*2\r\n"
    rf.readline(); c = int(rf.readline())
    n = int(rf.readline()[1:]); keys = []
    for _ in range(n):
        rf.readline(); keys.append(rf.readline().strip().decode())
    return c, keys
seen, cur, pages = set(), 0, 0
while True:
    cur, ks = scan_resp(cur); seen.update(ks); pages += 1
    if not cur or pages == 8:
        break
mid = len(seen)
rz = call("resize", col="0", buckets_log2=16)
ok = settled(65536)
while cur:
    cur, ks = scan_resp(cur); seen.update(ks)
check(ok and "result" in rz, "grow: paused at %d keys after 8 SCAN pages, resized to 65,536 buckets" % mid)
check(KEYS <= seen, "grow: SCAN returned %d of the 20,000 keys (every one at least once)" % len(KEYS & seen))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$D/res"
[ -s "$D/res" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
echo "scanresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
