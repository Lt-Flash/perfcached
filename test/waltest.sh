#!/bin/sh
# waltest.sh — S13 verification over a live daemon:
#  - every mutation class lands as a record (upsert/del/touch, add as the
#    ABSOLUTE resulting value), seq ascending, CRC-valid, wall-clock TTL;
#  - segments are zero-write provisioned (full-size, non-sparse);
#  - recycling wraps generations and stale-gen records are fenced;
#  - a torn/corrupted record stops the scan (red proof of the CRC);
#  - a tiny ring under a burst DROPS + counts, the daemon never blocks;
#  - stats carry the wal block; clean shutdown syncs the tail.
# Usage: test/waltest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwal.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

mkconf() { # mkconf <ring_kb> <segment_mb> <segments>
	mkdir -p "$D/wal"
	cat > "$D/w.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = wal-client-secret
cluster = wal-cluster-secret
[listen]
tcp = 127.0.0.1:16485
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
ring_kb = $1
segment_mb = $2
segments = $3
[collection th]
buckets_log2 = 10
EOF
}

start() {
	"$BIN" -f "$D/w.conf" > "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; cat "$D/log"; exit 1
}
stop() { kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }

# ---- round 1: record correctness --------------------------------------
mkconf 1024 1 4
start
grep -q "wal: active" "$D/log" && ok || bad "wal not active"
# provisioning: full-size and NOT sparse
SZ=$(stat -c%s "$D/wal/wal-000.seg")
AL=$(du -k "$D/wal/wal-000.seg" | cut -f1)
[ "$SZ" -eq $((1024*1024)) ] && ok || bad "segment size $SZ"
[ "$AL" -ge 1000 ] && ok || bad "segment sparse (du ${AL}k)"

python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16485), timeout=3)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
call("set", col="th", key="alpha", value="one")
call("set", col="th", key="beta", value="two", ttl=3600)
call("add", col="th", key="ctr", by=41)          # -> 41
call("add", col="th", key="ctr")                 # -> 42 (absolute!)
call("expire", col="th", key="alpha", ttl=100)   # touch
call("del", col="th", key="beta")
st = call("stats")["result"]["wal"]
assert st["fsync"] == "everysec" and st["last_seq"] >= 6, st
print("verbs done, wal stats:", st)
EOF
[ $? -eq 0 ] && ok || bad "verb phase"
stop
grep -q "wal: shutdown" "$D/log" && ok || bad "no clean wal shutdown"

R=$("$BIN" -W "$D/wal")
echo "--- wal dump ---"; echo "$R"
echo "$R" | grep -q "seq-order=ascending" && ok || bad "seq order"
echo "$R" | grep -q "stop=end" && ok || bad "stop reason: $R"
[ "$(echo "$R" | grep -c '^seq=')" -eq 6 ] && ok || bad "record count"
echo "$R" | grep -q "upsert th/ctr" && ok || bad "ctr upsert"
echo "$R" | grep -qE "val=42" && ok || bad "add logged ABSOLUTE 42"
echo "$R" | grep -q "del th/beta" && ok || bad "del record"
echo "$R" | grep -q "touch th/alpha" && ok || bad "touch record"
# beta's upsert carried a wall-clock expiry (10-digit epoch)
echo "$R" | grep -qE "upsert th/beta vlen=3 exp=1[0-9]{9}" && ok \
	|| bad "wall-clock ttl"

# ---- red: corrupt a record byte -> torn stop --------------------------
python3 - "$D/wal" <<'EOF'
import glob, struct, sys
# corrupt the ACTIVE segment (valid magic, lowest gen with content) -
# a freshly provisioned segment has no magic and is never scanned
best = None
for seg in sorted(glob.glob(sys.argv[1] + "/wal-*.seg")):
    with open(seg, "rb") as f:
        magic, gen = struct.unpack("<II", f.read(8))
    if magic == 0x4C574350 and (best is None or gen < best[1]):
        best = (seg, gen)
with open(best[0], "r+b") as f:
    f.seek(30)                       # inside the first record's payload
    b = f.read(1)
    f.seek(30); f.write(bytes([b[0] ^ 0xFF]))
EOF
R=$("$BIN" -W "$D/wal")
echo "$R" | grep -q "stop=torn" && ok || bad "corruption undetected: $R"

# ---- round 2: recycling + gen fencing ---------------------------------
rm -rf "$D/wal"; mkconf 1024 1 2; start
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16485), timeout=5)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
# ~3MB of records through 2x1MB segments -> recycling must happen
for i in range(700):
    call("set", col="th", key="k%03d" % (i % 100), value="x" * 4000)
st = call("stats")["result"]["wal"]
assert st["recycles"] >= 1, st
assert st["dropped"] == 0, st
print("recycles:", st["recycles"])
EOF
[ $? -eq 0 ] && ok || bad "recycling phase"
stop
R=$("$BIN" -W "$D/wal")
echo "$R" | grep -qE "stop=(end|stale-gen)" && ok || bad "post-recycle scan: $R"
N=$(echo "$R" | grep -c '^seq=')
[ "$N" -gt 50 ] && ok || bad "window too small after recycle ($N)"

# ---- round 3: tiny ring under a pipelined burst -> drops, no blocking --
rm -rf "$D/wal"; mkconf 4 1 4; start
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16485), timeout=5)
f = s.makefile("rwb")
# one pipelined burst: 2000 sets in a single write laps the 4KB ring
# no matter how the pump timing falls (500 once passed with 0 drops)
reqs = b"".join(
    (json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":"b%03d"%i,"value":"y"*900}})+"\n").encode()
    for i in range(2000))
f.write(reqs); f.flush()
for _ in range(2000):
    json.loads(f.readline())
rid=[1000]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
st = call("stats")["result"]["wal"]
assert st["dropped"] > 0, st
# the daemon is alive and serving - drops shed, never block
assert call("ping")["result"]["pong"] is True
print("dropped:", st["dropped"])
EOF
[ $? -eq 0 ] && ok || bad "drop-not-block phase"
stop

echo "waltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
