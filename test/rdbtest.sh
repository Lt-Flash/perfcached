#!/bin/sh
# rdbtest.sh — S14 verification: snapshots trigger by rule and by verb,
# land atomically (rename + dir fsync, no temp debris), validate by CRC,
# skip expired entries, honour the writer's rate budget, and record the
# WAL marker consistency needs.  Reds: corrupt snapshot detected; rules
# that should NOT fire, don't.  Usage: test/rdbtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrdb.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
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
import json, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
for i in range(200):
    call("set", col="th", key="k%03d"%i, value="v"*100)
call("set", col="th", key="dying", value="x", ttl=1)
time.sleep(1.5)                       # dying expires (unswept is fine)
# rule "2 1": within ~4s a save must fire
deadline = time.time() + 8
while time.time() < deadline:
    st = call("stats")["result"]["rdb"]
    if st["saves"] >= 1: break
    time.sleep(0.3)
assert st["saves"] >= 1, st
assert st["last_marker"] >= 200, st
print("rule save ok:", st)
EOF
[ $? -eq 0 ] && ok || bad "rule-triggered save"
[ -f "$D/wal/dump.rdb" ] && ok || bad "dump.rdb missing"
ls "$D/wal"/dump.rdb.tmp.* 2>/dev/null && bad "temp debris left" || ok
R=$("$BIN" -R "$D/wal")
echo "$R"
echo "$R" | grep -q "crc ok" && ok || bad "validate: $R"
N=$(echo "$R" | sed -n 's/rdb: \([0-9]*\) records.*/\1/p')
# 200 live + ctr-less; the dying key must have been skipped at write
[ "$N" -eq 200 ] && ok || bad "record count $N (expired not skipped?)"
stop

# ---- round 2: the save verb + a rate budget that must slow the writer --
rm -rf "$D/wal"; mkconf off 1; start      # no rules: manual only; 1 MB/s
client <<'EOF'
import json, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=10)
f = s.makefile("rwb"); rid=[0]
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
[ $? -eq 0 ] && ok || bad "save verb + rate budget"
stop

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
import json, socket, time
s = socket.create_connection(("127.0.0.1", 16486), timeout=5)
f = s.makefile("rwb")
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
