#!/bin/sh
# rdbtest.sh - snapshots by rule and by verb: atomic, CRC-validated, expired entries skipped, rate
# budget honoured, WAL marker recorded; corruption detected.  Usage: test/rdbtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrdb.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

mkconf() { # mkconf <save-rules> <rdb_mb_s>
	mkdir -p "$D/wal"
	cat > "$D/r.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = rdb-client-secret
cluster = rdb-cluster-secret
enable = rdb-enable-secret
[listen]
tcp = 127.0.0.1:16486
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 4
segments = 4
save = $1
rdb_mb_s = $2
[collection th]
buckets_log2 = 10
EOF
}

start() {
	"$BIN" -f "$D/r.conf" > "$D/log" 2>&1 &
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

client() { python3 - "$@"; }

# ---- round 1: rule-triggered save, contents, marker, expiry skip -------
mkconf "2 1" 0
start
client <<'EOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=5)
f = pcnative.wrap(s); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
# dying FIRST: rule "2 1" may save 2 s after the start, and 200 sets on a slow host
# can take long enough for the save to catch dying alive
call("set", col="th", key="dying", value="x", ttl=1)
for i in range(200):
    call("set", col="th", key="k%03d"%i, value="v"*100)
# a save covering all 201 writes may fire before dying expires: wait until it has, THEN
# write a trigger, so the save covering it (marker 202) comes after the expiry
time.sleep(2.5)                       # dying expired (unswept is fine)
call("set", col="th", key="trigger", value="t")
# wait on the COMPLETED snapshot: last_marker is stamped when a save's walk starts,
# safe_marker only once its rename has landed
deadline = time.time() + 10
while time.time() < deadline:
    st = call("stats")["result"]["rdb"]
    if st["safe_marker"] >= 202 and not st["running"]: break
    time.sleep(0.3)
assert st["saves"] >= 1, st
assert st["safe_marker"] >= 202 and not st["running"], st
print("rule save ok:", st)
EOF
[ $? -eq 0 ] && ok || bad "rule-triggered save"
[ -f "$D/wal/dump.rdb" ] && ok || bad "dump.rdb missing"
ls "$D/wal"/dump.rdb.tmp.* 2>/dev/null && bad "temp debris left" || ok
R=$("$BIN" -R "$D/wal")
echo "$R"
echo "$R" | grep -q "crc ok" && ok || bad "validate: $R"
N=$(echo "$R" | sed -n 's/rdb: \([0-9]*\) records.*/\1/p')
# 200 live + the trigger; the dying key must have been skipped at write
[ "$N" -eq 201 ] && ok || bad "record count $N (expired not skipped?)"
stop

# ---- round 2: the save verb + a rate budget that must slow the writer --
rm -rf "$D/wal"; mkconf off 1; start      # no rules: manual only; 1 MB/s
client <<'EOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=10)
f = pcnative.wrap(s); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
# ~3MB of data
for i in range(750):
    call("set", col="th", key="big%04d"%i, value="z"*4000)
# rules are off: nothing may have fired on its own
st = call("stats")["result"]["rdb"]
assert st["saves"] == 0, st
r = call("save")["result"]
assert r["started"] is True, r
deadline = time.time() + 15
while time.time() < deadline:
    st = call("stats")["result"]["rdb"]
    if st["saves"] == 1 and not st["running"]: break
    time.sleep(0.3)
assert st["saves"] == 1, st
# ~3MB at a 1 MB/s budget: the writer must have taken >= ~2s
assert st["last_dur_ms"] >= 2000, st
assert st["last_bytes"] > 2500000, st
print("verb save ok:", st)
EOF
[ $? -eq 0 ] && ok || { bad "save verb + rate budget"; echo "--- the daemon's log:"; tail -20 "$D/log"; }
stop

# ---- round 3: a snapshot larger than one read() returns ----------------
# Linux hands at most ~2 GiB to a read(); the test knob caps each at 64 KB, so this ~3 MB
# snapshot takes many: it must load whole, not as "corrupt" with the WAL alone left
PERFCACHED_TEST_READ_MAX=65536; export PERFCACHED_TEST_READ_MAX
R=$("$BIN" -R "$D/wal" 2>&1)
echo "$R" | grep -q "crc ok" && ok || bad "validate, 64 KB a read: $R"
start
client <<'EOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16486), timeout=10)
f = pcnative.wrap(s)
f.write(json.dumps({"jsonrpc":"2.0","id":1,"method":"get","params":{"col":"th","key":"big0749"}}).encode()+b"\n"); f.flush()
r = json.loads(f.readline())["result"]
assert r.get("found") and r["value"] == "z"*4000, r
EOF
[ $? -eq 0 ] && ok || bad "a 3 MB snapshot read 64 KB at a time did not load: $(grep -i 'snapshot\|recover' "$D/log" | tail -3 | tr '\n' '|')"
grep -q "missing pieces or corrupt" "$D/log" && bad "the snapshot was judged corrupt for needing more than one read()" || ok
stop
unset PERFCACHED_TEST_READ_MAX

# ---- round 4: a snapshot whose writes fail part-way ---------------------
# the test knob fails every snapshot write while its file exists, as a full volume does: the save
# must end as failed (counted, temp removed), not spin on the write with the thread held for ever
rm -rf "$D/wal"; mkconf off 0
PERFCACHED_TEST_RDB_WRITE_FAIL="$D/wfail"; export PERFCACHED_TEST_RDB_WRITE_FAIL
start
client "$D/wfail" "$D/wal" <<'EOF'
import glob, json, os, pcnative, socket, sys, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=10)
f = pcnative.wrap(s); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
for i in range(750):
    call("set", col="th", key="big%04d"%i, value="z"*4000)
open(sys.argv[1], "w").close()
assert call("save")["result"]["started"] is True
deadline = time.time() + 10
while time.time() < deadline:
    st = call("stats")["result"]["rdb"]
    if st["save_errors"] >= 1 and not st["running"]: break
    time.sleep(0.3)
assert st["save_errors"] >= 1 and not st["running"], ("the failed save never ended", st)
tmp = glob.glob(sys.argv[2] + "/dump.rdb.tmp.*")
assert not tmp, ("the failed save left its temp", tmp)
os.unlink(sys.argv[1])
assert call("save")["result"]["started"] is True
deadline = time.time() + 10
while time.time() < deadline:
    st = call("stats")["result"]["rdb"]
    if st["saves"] >= 1 and not st["running"]: break
    time.sleep(0.3)
assert st["saves"] >= 1, ("no save after the volume came back", st)
print("failed write ok:", st)
EOF
[ $? -eq 0 ] && ok || { bad "a snapshot write that fails part-way: the save must fail and end"; grep -i "rdb" "$D/log" | tail -3; }
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
unset PERFCACHED_TEST_RDB_WRITE_FAIL

# ---- round 5: a sealed snapshot whose record lengths overrun the file ----
# the CRC is not keyed: a crafted file passes it.  The loader and the load verb must bound each
# record against what is left, as -R does, and refuse the file, not read past it
mksnap() { # mksnap <file> <bad: neg | over>
	python3 - "$1" "$2" <<'EOF'
import struct, sys, time
T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
    T.append(c)
def crc32c(b):
    c = 0xFFFFFFFF
    for x in b:
        c = T[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF
def rec(k, v, kl=None, vl=None):
    return (bytes([2]) + struct.pack("<IIQQ", len(k) if kl is None else kl,
        len(v) if vl is None else vl, 0, 7) + b"\0" + k + v)
d = struct.pack("<IIQQ", 0x44524350, 3, 0, int(time.time()))
d += bytes([1, 2]) + b"th" + rec(b"good", b"g1")
if sys.argv[2] == "neg":
    d += rec(b"", b"", kl=0xFFFFFFFF, vl=0)        # a key of 4 GiB - 1, as int -1
else:
    d += rec(b"evil", b"0123456789", vl=200000)    # 200 KB claimed, 10 bytes there
d += b"\xff"
d += struct.pack("<I", crc32c(d))
open(sys.argv[1], "wb").write(d)
EOF
}
trystart() {
	"$BIN" -f "$D/r.conf" > "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	return 1
}
getv() { # getv <key>: the value, or None
	python3 - "$1" <<'EOF'
import json, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16486), timeout=5))
f.write(json.dumps({"jsonrpc":"2.0","id":1,"method":"get","params":{"col":"th","key":sys.argv[1]}}).encode()+b"\n"); f.flush()
r = json.loads(f.readline())["result"]
print(len(r["value"]) if r.get("found") else None)
EOF
}
for bad in neg over; do
	rm -rf "$D/wal"; mkconf off 0
	mksnap "$D/wal/dump.rdb" $bad
	"$BIN" -R "$D/wal" >/dev/null 2>&1 && bad "5 $bad: -R accepted the overrunning record" || ok
	if trystart; then
		grep -q "missing pieces or corrupt" "$D/log" && ok ||
			bad "5 $bad: a snapshot whose record overruns the file was loaded, not refused"
		[ "$bad" = over ] && { v=$(getv evil); [ "$v" = None ] && ok || bad "5 over: the overrunning value was stored ($v bytes)"; }
	else
		bad "5 $bad: the daemon did not start on a snapshot whose record overruns the file: $(tail -2 "$D/log" | tr '\n' '|')"
	fi
	kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
done
# the load verb reads the same file on a running node
rm -rf "$D/wal"; mkconf off 0; start
mksnap "$D/wal/dump.rdb" over
client <<'EOF'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16486), timeout=10))
def call(m, **p):
    r={"jsonrpc":"2.0","id":1,"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
assert "result" in call("enable", secret="rdb-enable-secret")
r = call("load")
g = call("get", col="th", key="evil")["result"]
assert "error" in r and "snapshot" in r["error"]["message"] and not g.get("found"), (r, g.get("found"))
EOF
[ $? -eq 0 ] && ok || bad "5: the load verb took a snapshot whose record overruns the file"
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=

# ---- reds --------------------------------------------------------------
# corrupt one byte -> validate must refuse
python3 - "$D/wal/dump.rdb" <<'EOF'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(100); b = f.read(1)
    f.seek(100); f.write(bytes([b[0] ^ 0xFF]))
EOF
"$BIN" -R "$D/wal" >/dev/null 2>&1 && bad "corrupt rdb accepted" || ok

# a rule that must NOT fire: 900s window with tiny traffic
rm -rf "$D/wal"; mkconf "900 1" 0; start
client <<'EOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=5)
f = pcnative.wrap(s)
f.write(b'{"jsonrpc":"2.0","id":1,"method":"set","params":{"col":"th","key":"a","value":"b"}}\n')
f.flush(); f.readline()
time.sleep(3)
f.write(b'{"jsonrpc":"2.0","id":2,"method":"stats"}\n'); f.flush()
st = json.loads(f.readline())["result"]["rdb"]
assert st["saves"] == 0, st
print("gate held:", st)
EOF
[ $? -eq 0 ] && ok || bad "trigger gate fired early"
stop

# ---- round 6: a rename, a snapshot, a kill -9 ---------------------------
# with a WAL a rename used to reach only the log: a snapshot taken after it names the collection
# by its new name, which the collections file did not know, and both halves were dropped
rm -rf "$D/wal" "$D/st"; mkconf off 0; mkdir -p "$D/st"
sed -i "s|^\[daemon\]|[daemon]\nstate_dir = $D/st\nallow_create = yes|" "$D/r.conf"
start
client <<'EOF'
import json, pcnative, socket, time
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16486), timeout=10)); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
def ok(r):
    assert "result" in r, r
    return r["result"]
def save():
    n = ok(call("stats"))["rdb"]["saves"]
    assert ok(call("save"))["started"] is True
    deadline = time.time() + 10
    while time.time() < deadline:
        st = ok(call("stats"))["rdb"]
        if st["saves"] > n and not st["running"]: return
        time.sleep(0.2)
    raise AssertionError(("no save", st))
def fill(col, pre):
    for i in range(20): ok(call("set", col=col, key=pre+"%02d"%i, value=col+pre))
ok(call("enable", secret="rdb-enable-secret"))
for c in ("a", "x", "p"): ok(call("create", col=c))
fill("a", "k"); fill("x", "k"); fill("p", "k")
ok(call("rename", col="a", to="b"))      # renamed, THEN snapshot: it names b
save()
fill("b", "m")
ok(call("rename", col="x", to="y")); ok(call("rename", col="y", to="z"))   # a chain
fill("z", "m")
ok(call("rename", col="p", to="q")); fill("q", "m")
ok(call("rename", col="q", to="p")); fill("p", "n")   # and back
ok(call("sync"))
EOF
[ $? -eq 0 ] && ok || bad "6: setup"
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
start
client <<'EOF'
import json, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16486), timeout=10)); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
bad = 0
# (where it must be now, the prefix, the value it was written with)
for col, pre, want in (("b", "k", "ak"), ("b", "m", "bm"), ("z", "k", "xk"), ("z", "m", "zm"),
        ("p", "k", "pk"), ("p", "m", "qm"), ("p", "n", "pn")):
    miss = [i for i in range(20) if call("get", col=col,
        key=pre+"%02d"%i).get("result", {}).get("value") != want]
    if miss:
        bad += 1
        print("FAIL: 6: %d of the %s* keys written as '%s' are not in '%s' after the restart"
            % (len(miss), pre, want, col))
cols = sorted(c["name"] for c in call("collections")["result"]["collections"])
for gone in ("a", "x", "y", "q"):
    if gone in cols:
        bad += 1
        print("FAIL: 6: the old name '%s' is back after the restart: %s" % (gone, cols))
sys.exit(1 if bad else 0)
EOF
[ $? -eq 0 ] && ok || { bad "6: a renamed collection did not come back whole after a kill -9"; grep -i "recover:" "$D/log" | tail -2; }
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=

# ---- round 7: load beside a client's write and another load --------------
# the import probed a key, then stored the snapshot's copy over whatever a client wrote meanwhile;
# PERFCACHED_TEST_IMPORT_GAP_MS holds it 1.5 s between a key's probe and its store
rm -rf "$D/wal" "$D/st"; mkconf off 0
export PERFCACHED_TEST_IMPORT_GAP_MS=1500
start
unset PERFCACHED_TEST_IMPORT_GAP_MS
client <<'EOF'
import json, pcnative, select, socket, sys, time
def conn():
    return pcnative.wrap(socket.create_connection(("127.0.0.1", 16486), timeout=20))
rid = [0]
def send(f, m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
def call(f, m, **p):
    send(f, m, **p)
    return json.loads(f.readline())
f = conn()
call(f, "set", col="th", key="r", value="old")
n = call(f, "stats")["result"]["rdb"]["saves"]
assert call(f, "save")["result"]["started"] is True
deadline = time.time() + 10
while time.time() < deadline:
    st = call(f, "stats")["result"]["rdb"]
    if st["saves"] > n and not st["running"]: break
    time.sleep(0.2)
assert st["saves"] > n, st
call(f, "del", col="th", key="r")
a = conn()
assert "result" in call(a, "enable", secret="rdb-enable-secret")
cands = [conn() for _ in range(12)]
send(a, "load")                  # probes r (absent), then holds 1.5 s before its store
time.sleep(0.3)
for c in cands: send(c, "ping")
time.sleep(0.4)
other = [c for c in cands if select.select([c], [], [], 0)[0]]
if not other:
    print("FAIL: 7: no connection reached the other worker"); sys.exit(1)
b = other[0]
b.readline()
call(b, "set", col="th", key="r", value="new")
assert "result" in call(b, "enable", secret="rdb-enable-secret")
second = call(b, "load")
first = json.loads(a.readline())
got = call(b, "get", col="th", key="r").get("result", {}).get("value")
bad = 0
if got != "new":
    bad += 1
    print("FAIL: 7: a write made while load ran was overwritten by the snapshot's older value "
        "(%r; the load said %r)" % (got, first))
if "error" not in second or "running" not in second["error"]["message"]:
    bad += 1
    print("FAIL: 7: a second load ran beside the first: %r" % second)
sys.exit(1 if bad else 0)
EOF
[ $? -eq 0 ] && ok || bad "7: load beside a client's write and a second load"
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=

echo "rdbtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
