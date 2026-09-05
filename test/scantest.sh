#!/bin/sh
# scantest.sh — S9 verification: keys/scan over a live daemon.
# The core obligation: a cursored scan loop run DURING a concurrent
# write/delete storm (with linear-hash growth happening underneath)
# returns every stable key at least once.  Plus: glob correctness against
# python's fnmatch as an independent reference, limit/truncated, values
# mode with binary keys+values, TTL filtering, and the red paths.
# Usage: test/scantest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT

cat > "$D/s9.conf" <<EOF
[daemon]
workers = 3
[memory]
arena_mb = 64
[secrets]
client = scan-client-secret
cluster = scan-cluster-secret
[listen]
tcp = 127.0.0.1:16483
plaintext = loopback
[collection th]
buckets_log2 = 8
EOF

"$BIN" -f "$D/s9.conf" > "$D/log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || { echo "FAIL: daemon died"; cat "$D/log"; exit 1; }
	sleep 0.1; i=$((i+1))
done

python3 - <<'EOF'
import json, socket, base64, sys, threading, time, fnmatch
fails = []; passes = 0
def check(name, cond, *ctx):
    global passes
    if cond: passes += 1; print("ok:", name)
    else: fails.append(name); print("FAIL:", name, *ctx)

class C:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 16483), timeout=5)
        self.f = self.s.makefile("rwb")
        self.id = 0
    def call(self, method, **params):
        self.id += 1
        req = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params: req["params"] = params
        self.f.write(json.dumps(req).encode() + b"\n"); self.f.flush()
        return json.loads(self.f.readline())

c = C()

# seed 5000 stable keys (batched msets) - starts at 256 buckets, so the
# maintenance thread MUST grow the table while the storm runs
STABLE = ["stable-%04d" % i for i in range(5000)]
for i in range(0, 5000, 250):
    r = c.call("mset", col="th",
               items=[{"key": k, "value": "v" + k} for k in STABLE[i:i+250]])
    assert r["result"]["stored"] == 250, r

# a churn thread stores/deletes volatile keys throughout the scans
stop = threading.Event()
def churn():
    cc = C(); n = 0
    while not stop.is_set():
        k = "churn-%03d" % (n % 500)
        cc.call("set", col="th", key=k, value="x" * (n % 200 + 1))
        if n % 3 == 2:
            cc.call("del", col="th", key=k)
        n += 1
t = threading.Thread(target=churn); t.start()

def full_scan(**kw):
    seen = []; cursor = 0
    while True:
        r = c.call("scan", col="th", cursor=cursor, count=64, **kw)["result"]
        seen += [it["k"] for it in r["items"]]
        cursor = r["cursor"]
        if not r["more"]:
            return seen

def buckets():
    return c.call("stats", col="th")["result"]["collections"][0]["buckets"]

try:
    # round 0 immediately, then WAIT for linear-hash growth to start (the
    # maintenance tick is 1/s), then two more rounds that provably overlap
    # live bucket splitting - the resize-stability half of the guarantee
    seen = set(full_scan())
    missing = [k for k in STABLE if k not in seen]
    check("storm scan 0: all stable keys seen", not missing,
          "missing %d e.g. %s" % (len(missing), missing[:3]))
    for _ in range(80):
        if buckets() > 256:
            break
        time.sleep(0.1)
    check("growth started under storm", buckets() > 256, buckets())
    for rnd in (1, 2):
        b0 = buckets()
        seen = set(full_scan())
        missing = [k for k in STABLE if k not in seen]
        check("storm scan %d (buckets %d->%d): all stable keys seen"
              % (rnd, b0, buckets()), not missing,
              "missing %d e.g. %s" % (len(missing), missing[:3]))
finally:
    stop.set(); t.join()

# glob correctness vs fnmatch as an independent reference
for pat in ["stable-00*", "stable-?9?9", "stable-[0-2]*", "stable-[^0-4]*",
            "stable-1234", "*-4999"]:
    r = c.call("keys", col="th", match=pat, limit=100000)["result"]
    got = set(k for k in r["keys"] if isinstance(k, str) and k.startswith("stable-"))
    # fnmatch has no [^..]; translate to its [!..] form for the reference
    want = set(k for k in STABLE if fnmatch.fnmatchcase(k, pat.replace("[^", "[!")))
    check("glob %s" % pat, got == want,
          "extra=%s missing=%s" % (list(got - want)[:3], list(want - got)[:3]))

# limit + truncated
r = c.call("keys", col="th", match="stable-*", limit=50)["result"]
check("limit honoured", len(r["keys"]) == 50 and r["truncated"] is True, r)
r = c.call("keys", col="th", match="no-such-*")["result"]
check("no match: empty, not truncated",
      r["keys"] == [] and r["truncated"] is False, r)

# values mode + a genuinely binary KEY (real NUL via the JSON escape in
# the request the client lib writes) and a binary VALUE (b64): both must
# ride b64 markers in the scan reply
raw_k = "zk\x00ey"           # json.dumps emits \u0000; the daemon stores a real NUL
raw_v = b"val\xff\x00!"
r = c.call("set", col="th", key=raw_k,
           value=base64.b64encode(raw_v).decode(), enc="b64")
check("binary key+value stored", r["result"]["stored"] is True, r)
r = c.call("scan", col="th", match="zk*", values=True, count=16384)["result"]
check("scan finds the NUL key", len(r["items"]) == 1, r)
it = r["items"][0] if r["items"] else {}
check("scan k_enc b64 round-trip",
      it.get("k_enc") == "b64" and
      base64.b64decode(it.get("k", "")) == raw_k.encode(), it)
check("scan value b64 round-trip",
      it.get("enc") == "b64" and base64.b64decode(it.get("v", "")) == raw_v, it)
check("scan item ttl -1", it.get("ttl") == -1, it)

# TTL filter: an expired-but-unswept key must not be listed
c.call("set", col="th", key="dying", value="x", ttl=1)
time.sleep(2.2)
r = c.call("keys", col="th", match="dying")["result"]
check("expired key filtered", r["keys"] == [], r)

# red paths
check("scan bad cursor", "error" in c.call("scan", col="th", cursor=-1))
check("scan count range", "error" in c.call("scan", col="th", count=999999))
check("keys limit range", "error" in c.call("keys", col="th", limit=0))
check("keys no such col", "error" in c.call("keys", col="nope"))

print("scantest: %d passed, %d failed" % (passes, len(fails)))
sys.exit(1 if fails else 0)
EOF
rc=$?

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
[ $rc -eq 0 ] && echo "scantest: PASS" || echo "scantest: FAILED"
exit $rc
