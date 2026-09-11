#!/bin/sh
# recoverytest.sh — S15 verification: a perfcached node survives death.
#  1. Full round-trip: dataset -> snapshot -> MORE writes (the WAL tail)
#     -> SIGKILL -> restart -> every key verified: values, counters
#     (absolute), deletes stayed dead, TTLs survived, expired stayed
#     expired.
#  2. Kill -9 storm: five generations of writes, each ended by SIGKILL
#     at a random moment; the survivor set must match the model.
#  3. Torn tail: garbage appended to the active segment after a kill -
#     restart succeeds, replay stops cleanly at the tear.
#  4. FAIL-FIRST: a corrupted mid-WAL record provably GATES replay (a
#     later write is lost exactly as the CRC dictates), and -W calls the
#     WAL torn before anything trusts it.
#  5. Corrupt snapshot: CRIT + cold start + WAL-alone rebuild.
# Usage: test/recoverytest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
# PCREC_TMP puts the whole fixture on a chosen filesystem: S62 only ever
# failed on slow storage, and without this the test cannot be pointed at
# any.  PCREC_KEEP leaves the directory (and its per-generation logs)
# behind for a post-mortem.
D=$(mktemp -d "${PCREC_TMP:-/var/tmp}/pcrec.XXXXXX")
PID=
# Startup includes the WAL probe, which on heavily throttled or very slow
# storage legitimately takes tens of seconds - measured at 29s under a
# cgroup io.max of 10 wiops, where the probe correctly reports ~5
# sustainable synced writes/s.  The old fixed 8s wait called that a dead
# daemon.  A generous default only costs time when a start really fails.
STARTMAX=$(( ${PCREC_START_WAIT:-45} * 10 ))
cleanup() {
	[ -n "$PID" ] && kill -9 $PID 2>/dev/null
	if [ -n "${PCREC_KEEP:-}" ]; then echo "kept: $D"; else rm -rf "$D"; fi
}
trap cleanup EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

mkconf() {
	mkdir -p "$D/wal"
	cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = rec-client-secret
cluster = rec-cluster-secret
[listen]
tcp = 127.0.0.1:16487
plaintext = loopback
[wal]
dir = $D/wal
# probe = auto, NOT no.  (No backticks below: this is an unquoted
# heredoc, and a backtick here is command substitution - the first
# draft of this comment made the shell try to RUN "probe".)
#
# This leg asserts that acked writes survive SIGKILL, and fsync = always
# only delivers that if the ring can absorb what arrives while the pump
# sits in fdatasync.  With probe = no the depth cannot be sized for the
# device, and the daemon says so on every start: dropped records are
# acknowledged writes that will be missing after a restart.
#
# MEASURED 2026-09-02, on a GitHub runner where this leg fails 2 of 20:
# fdatasync p50 0.60ms but p99 148.86ms, max 181.53ms - a 250x tail.
# On the NVMe build host the same leg passes ~50 for 50, which is why
# it looked like a CI mystery for three failures rather than a config
# the daemon had already warned about.
probe = auto
fsync = always
segment_mb = 2
segments = 4
save = off
[collection th]
buckets_log2 = 10
[collection cnt]
buckets_log2 = 8
EOF
}

start() {
	"$BIN" -f "$D/c.conf" >> "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt $STARTMAX ]; do
		grep -q "perfcached ready" "$D/log" && { : > /dev/null; return 0; }
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; tail -5 "$D/log"; exit 1
}
# Each start appends.  Rounds used to share ONE log truncated between
# them, which is fine for a clean grep and catastrophic for a failure
# you cannot reproduce: by the time the storm asserted, four of the five
# daemons' logs had been overwritten.  Keep them, numbered.
LOGN=0
newlog() {
	LOGN=$((LOGN + 1))
	D_LOG="$D/log$LOGN"
	: > "$D_LOG"
	ln -sf "$D_LOG" "$D/log"       # existing greps still see "$D/log"
}
D_LOG="$D/log"; : > "$D_LOG"
hardkill() { kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }

mkconf

# ---- 1. the full round-trip -------------------------------------------
newlog; start
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
for i in range(300):
    call("set", col="th", key="pre%03d"%i, value="snapval%03d"%i)
call("add", col="cnt", key="hits", by=100)
call("set", col="th", key="doomed", value="x")
call("set", col="th", key="longttl", value="keepme", ttl=3600)
call("set", col="th", key="shortttl", value="dropme", ttl=1)
assert call("save")["result"]["started"] is True
import time
while call("stats")["result"]["rdb"]["saves"] < 1:
    time.sleep(0.2)
# THE WAL TAIL: everything after the snapshot
for i in range(50):
    call("set", col="th", key="post%02d"%i, value="tailval%02d"%i)
call("add", col="cnt", key="hits", by=11)      # 111 absolute
call("del", col="th", key="doomed")
call("expire", col="th", key="longttl", ttl=7200)
time.sleep(1.5)                                 # shortttl dies; fsync=always
EOF
[ $? -eq 0 ] && ok || bad "phase-1 writes"
hardkill

newlog; start
grep -q "recover: rdb loaded" "$D/log" && ok || bad "rdb not loaded"
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
import sys
bad = []
for i in range(300):
    r = call("get", col="th", key="pre%03d"%i)["result"]
    if not r.get("found") or r["value"] != "snapval%03d"%i: bad.append(i)
for i in range(50):
    r = call("get", col="th", key="post%02d"%i)["result"]
    if not r.get("found") or r["value"] != "tailval%02d"%i: bad.append("p%d"%i)
assert not bad, bad[:5]
assert call("add", col="cnt", key="hits", by=0)["result"]["value"] == 111
assert call("get", col="th", key="doomed")["result"]["found"] is False
t = call("ttl", col="th", key="longttl")["result"]["ttl"]
assert 7000 < t <= 7200, t
assert call("get", col="th", key="shortttl")["result"]["found"] is False
print("round-trip verified: 350 keys, counter 111, delete dead, ttl", t)
EOF
[ $? -eq 0 ] && ok || bad "phase-1 verification"
hardkill

# ---- 2. the kill -9 storm ---------------------------------------------
rm -rf "$D/wal"; mkconf
for gen in 0 1 2 3 4; do
	newlog; start
	python3 - $gen <<'EOF'
import json, socket, sys, random, time
gen = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
for i in range(120):
    call("set", col="th", key="g%d-%03d"%(gen,i), value="gen%dval"%gen)
call("add", col="cnt", key="gens", by=1)
EOF
	[ $? -eq 0 ] || bad "storm gen $gen writes"
	sleep 0.$((RANDOM % 5 + 3)) 2>/dev/null || sleep 0.4
	hardkill
done
newlog; start
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
missing = 0
for gen in range(5):
    for i in range(120):
        r = call("get", col="th", key="g%d-%03d"%(gen,i))["result"]
        if not r.get("found") or r["value"] != "gen%dval"%gen:
            missing += 1
assert missing == 0, "%d keys lost across 5 kill-9 generations" % missing
assert call("add", col="cnt", key="gens", by=0)["result"]["value"] == 5
print("storm verified: 600 keys + counter across 5 SIGKILLs")
EOF
if [ $? -eq 0 ]; then
	ok
else
	bad "storm verification"
	# SHOW THE DAEMON'S OWN WORDS.  This leg has failed intermittently
	# on CI three times and been mute every time, because the daemon
	# logs land in $D and CI only ever sees the assertion.  The WAL has
	# a great deal to say about lost records - "wal: FULL - no free
	# segment ... no longer replayable", "wal: DROPPED %llu
	# acknowledged write(s)" - and any of it would have turned three
	# unexplained reds into a diagnosis.
	echo "--- what the daemons said (one log per generation) ---"
	for L in "$D"/log[0-9]*; do
		[ -f "$L" ] || continue
		M=$(grep -iE "wal:|WARN|ERR|CRIT|snapshot|segment|replay" "$L" \
			| tail -6)
		[ -n "$M" ] && { echo "  == $(basename "$L")"
		                 echo "$M" | sed 's/^/    /'; }
	done
fi
hardkill

# ---- 3+4. torn tail + the fail-first gate ------------------------------
newlog; start
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb")
for i, (k, v) in enumerate([("keepA","1"),("keepB","2"),("lostC","3")]):
    f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":k,"value":v}}).encode()+b"\n")
f.flush()
for _ in range(3): f.readline()
EOF
hardkill
# corrupt the SECOND record (keepB stays valid, lostC is beyond the tear)
python3 - "$D/wal" <<'EOF'
import glob, struct, sys
best = None
for seg in glob.glob(sys.argv[1] + "/wal-*.seg"):
    with open(seg, "rb") as f:
        magic, gen = struct.unpack("<II", f.read(8))
    if magic == 0x4C574350 and (best is None or gen > best[1]):
        best = (seg, gen)
seg = best[0]
with open(seg, "rb") as f:
    data = f.read()
off = 8
recs = []
while True:
    ln = struct.unpack("<I", data[off:off+4])[0]
    if ln == 0: break
    recs.append(off)
    off += 12 + ln
assert len(recs) >= 3, recs
tgt = recs[-2]                       # the middle of the last three
with open(seg, "r+b") as f:
    f.seek(tgt + 20)
    b = f.read(1)
    f.seek(tgt + 20); f.write(bytes([b[0] ^ 0xFF]))
EOF
"$BIN" -W "$D/wal" | grep -q "stop=torn" && ok || bad "-W missed the tear"
newlog; start
grep -qE "wal replay [0-9]+ applied" "$D/log" && ok || bad "no replay line"
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
# fail-first: the CRC gate must have cut replay AT the corrupted record
assert call("get", col="th", key="keepA")["result"]["found"] is True
assert call("get", col="th", key="keepB")["result"]["found"] is False
assert call("get", col="th", key="lostC")["result"]["found"] is False
print("tear gated replay exactly where the CRC said")
EOF
[ $? -eq 0 ] && ok || bad "tear gating"
hardkill

# ---- 5. corrupt snapshot: cold start, WAL-alone rebuild ----------------
rm -rf "$D/wal"; mkconf
newlog; start
python3 - <<'EOF'
import json, socket, time
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
call("set", col="th", key="walonly", value="survives")
call("save")
while call("stats")["result"]["rdb"]["saves"] < 1:
    time.sleep(0.2)
call("set", col="th", key="tail", value="alsohere")
time.sleep(0.3)
EOF
hardkill
python3 - "$D/wal/dump.rdb" <<'EOF'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(30); b = f.read(1)
    f.seek(30); f.write(bytes([b[0] ^ 0xFF]))
EOF
newlog; start
grep -q "starting from the WAL alone" "$D/log" && ok || bad "no cold-start CRIT"
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = s.makefile("rwb")
f.write(b'{"jsonrpc":"2.0","id":1,"method":"get","params":{"col":"th","key":"walonly"}}\n')
f.write(b'{"jsonrpc":"2.0","id":2,"method":"get","params":{"col":"th","key":"tail"}}\n')
f.flush()
import sys
r1 = json.loads(f.readline())["result"]
r2 = json.loads(f.readline())["result"]
assert r1["found"] and r1["value"] == "survives", r1
assert r2["found"] and r2["value"] == "alsohere", r2
print("cold start rebuilt from the WAL alone")
EOF
[ $? -eq 0 ] && ok || bad "wal-alone rebuild"
hardkill

echo "recoverytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
