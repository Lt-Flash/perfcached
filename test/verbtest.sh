#!/bin/sh
# verbtest.sh — S8 verification: the full v1 verb set over a live daemon
# (text dialect).  Covers hit/miss, TTL semantics, counters, binary
# values via b64, batch ops, stats, and the error paths.
# Usage: test/verbtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT

cat > "$D/v.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = verb-client-secret
cluster = verb-cluster-secret
[listen]
tcp = 127.0.0.1:16482
plaintext = loopback
[collection th]
buckets_log2 = 12
[collection cnt]
buckets_log2 = 10
EOF

"$BIN" -f "$D/v.conf" > "$D/log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || { echo "FAIL: daemon died"; cat "$D/log"; exit 1; }
	sleep 0.1; i=$((i+1))
done

python3 - <<'EOF'
import json, socket, base64, sys, time
fails = []; passes = 0
def check(name, cond, *ctx):
    global passes
    if cond: passes += 1; print("ok:", name)
    else: fails.append(name); print("FAIL:", name, *ctx)

class C:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 16482), timeout=3)
        self.f = self.s.makefile("rwb")
        self.id = 0
    def call(self, method, **params):
        self.id += 1
        req = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params: req["params"] = params
        self.f.write(json.dumps(req).encode() + b"\n"); self.f.flush()
        return json.loads(self.f.readline())

c = C()

# set / get roundtrip + ttl reported
r = c.call("set", col="th", key="k1", value="hello")
check("set", r["result"]["stored"] is True, r)
r = c.call("get", col="th", key="k1")
check("get hit", r["result"]["found"] and r["result"]["value"] == "hello", r)
check("get ttl -1 (no expiry)", r["result"]["ttl"] == -1, r)

# miss
r = c.call("get", col="th", key="absent")
check("get miss", r["result"]["found"] is False, r)

# exists
check("exists true", c.call("exists", col="th", key="k1")["result"]["exists"], )
check("exists false", c.call("exists", col="th", key="absent")["result"]["exists"] is False)

# ttl semantics: -2 absent, -1 no-expiry, positive with expiry
check("ttl absent -2", c.call("ttl", col="th", key="absent")["result"]["ttl"] == -2)
check("ttl no-expiry -1", c.call("ttl", col="th", key="k1")["result"]["ttl"] == -1)
c.call("set", col="th", key="ephemeral", value="x", ttl=100)
t = c.call("ttl", col="th", key="ephemeral")["result"]["ttl"]
check("ttl positive", 90 <= t <= 100, t)

# expire re-arms without rewriting
check("expire updated", c.call("expire", col="th", key="k1", ttl=50)["result"]["updated"])
check("expire on absent", c.call("expire", col="th", key="absent", ttl=50)["result"]["updated"] is False)
check("expire took", 40 <= c.call("ttl", col="th", key="k1")["result"]["ttl"] <= 50)

# del
check("del present", c.call("del", col="th", key="k1")["result"]["deleted"])
check("del absent", c.call("del", col="th", key="k1")["result"]["deleted"] is False)

# add / sub counters
check("add default 1", c.call("add", col="cnt", key="c")["result"]["value"] == 1)
check("add by 41", c.call("add", col="cnt", key="c", by=41)["result"]["value"] == 42)
check("sub by 2", c.call("sub", col="cnt", key="c", by=2)["result"]["value"] == 40)
# add on a non-integer value errors
c.call("set", col="th", key="str", value="notanumber")
r = c.call("add", col="th", key="str")
check("add non-integer errors", "error" in r, r)

# binary value via b64 (NUL + high byte), round-trips as b64
raw = b"bin\x00\xffval"
r = c.call("set", col="th", key="bk",
           value=base64.b64encode(raw).decode(), enc="b64")
check("set b64", r["result"]["stored"])
r = c.call("get", col="th", key="bk")["result"]
check("get b64 round-trip", r["enc"] == "b64" and base64.b64decode(r["value"]) == raw, r)

# mget mixed hit/miss, order preserved
c.call("set", col="th", key="a", value="A")
c.call("set", col="th", key="b", value="B")
r = c.call("mget", col="th", keys=["a", "absent", "b"])["result"]["values"]
check("mget order+mix",
      r[0]["value"] == "A" and r[1]["found"] is False and r[2]["value"] == "B", r)

# mset counts stored
r = c.call("mset", col="th", items=[
    {"key": "m1", "value": "1"}, {"key": "m2", "value": "2", "ttl": 60}])["result"]
check("mset stored 2", r["stored"] == 2 and r["dropped"] == 0, r)
check("mset visible", c.call("get", col="th", key="m2")["result"]["value"] == "2")

# stats reflects the collection
st = c.call("stats", col="th")["result"]["collections"]
check("stats one col", len(st) == 1 and st[0]["name"] == "th", st)
check("stats counters move", st[0]["stores"] > 0 and st[0]["entries"] > 0, st)
allst = c.call("stats")["result"]["collections"]
check("stats all cols", {x["name"] for x in allst} == {"th", "cnt"}, allst)

# error paths
check("no such collection", "error" in c.call("get", col="nope", key="x"))
check("missing key param", "error" in c.call("get", col="th"))
check("unknown method", c.call("frobnicate")["error"]["code"] == -32601)

print("verbtest: %d passed, %d failed" % (passes, len(fails)))
sys.exit(1 if fails else 0)
EOF
rc=$?

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
[ $rc -eq 0 ] && echo "verbtest: PASS" || echo "verbtest: FAILED"
exit $rc
