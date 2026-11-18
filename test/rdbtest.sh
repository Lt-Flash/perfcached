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

echo "rdbtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
