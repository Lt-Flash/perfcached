#!/bin/sh
# recoverytest.sh - a node survives death: snapshot + WAL tail round-trip, a kill -9 storm, a torn
# tail, a corrupted mid-WAL record that gates replay, and a corrupt snapshot rebuilt from the WAL.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
# PCREC_TMP puts the whole fixture on a chosen filesystem (slow storage is where it matters);
# PCREC_KEEP leaves the directory and its per-generation logs behind
D=$(mktemp -d "${PCREC_TMP:-/var/tmp}/pcrec.XXXXXX")
PID=
# startup includes the WAL probe, which on throttled storage legitimately takes tens of seconds
STARTMAX=$(( ${PCREC_START_WAIT:-45} * 10 ))
cleanup() {
	[ -n "$PID" ] && kill -9 $PID 2>/dev/null
	if [ -n "${PCREC_KEEP:-}" ]; then echo "kept: $D"; else rm -rf "$D"; fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
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
# probe = auto, NOT no: acked writes survive SIGKILL only if the ring, sized for the
# device by the probe, absorbs what arrives while the pump sits in fdatasync
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
# each start appends to its own numbered log, so a storm failure keeps every daemon's words
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
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
import json, pcnative, socket, sys, random, time
gen = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
	# an ack is at the producer ring, not the file (src/wal.h), and kill -9 takes what a ring holds:
	# wait on wal.staged, bounded, and say so if it never drains
	w=0
	while [ $w -lt 100 ]; do
		st=$(python3 - <<'PYS'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5); f = pcnative.wrap(s)
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
w = json.loads(f.readline())["result"]["wal"]
print(w.get("staged", (w["last_seq"] - w["synced_seq"])))
PYS
)
		[ "${st:-1}" = 0 ] && break
		sleep 0.1; w=$((w+1))
	done
	[ "${st:-1}" = 0 ] && echo "  gen $gen: wal drained (staged=0 after $((w/10)).$((w%10))s)" \
		|| bad "gen $gen: the wal never drained in 10s (staged=$st) - the storm cannot assert no-loss"
	hardkill
done
newlog; start
python3 - <<'EOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
	# show the daemons' own words: the WAL logs every lost record ("wal: FULL", "wal: DROPPED")
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
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s)
for i, (k, v) in enumerate([("keepA","1"),("keepB","2"),("lostC","3")]):
    f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":k,"value":v}}).encode()+b"\n")
f.flush()
for _ in range(3): f.readline()
EOF
hardkill
# corrupt keepB's own record, found by its key: a segment may rotate between the three writes
# (keepA stays valid, lostC is beyond the tear)
python3 - "$D/wal" > "$D/tear.txt" <<'EOF'
import glob, struct, sys
def records(path):          # (offset, collection, key); bounded - a just-rotated segment may hold none
    data = open(path, "rb").read()
    off, out = 8, []
    while off + 12 <= len(data):
        ln = struct.unpack("<I", data[off:off+4])[0]
        if ln < 8 or off + 12 + ln > len(data): break
        p = data[off+12:off+12+ln]
        cl, kl, vl = p[1], struct.unpack("<H", p[2:4])[0], struct.unpack("<I", p[4:8])[0]
        h = ln - cl - kl - vl
        out.append((off, p[h:h+cl], p[h+cl:h+cl+kl]))
        off += 12 + ln
    return out
cands = []
for seg in glob.glob(sys.argv[1] + "/wal-*.seg"):
    with open(seg, "rb") as f:
        h = f.read(8)
    if len(h) == 8 and struct.unpack("<I", h[:4])[0] == 0x4C574350:
        cands.append((struct.unpack("<I", h[4:])[0], seg))
seg = tgt = None
for gen, path in sorted(cands, reverse=True):   # keepB's newest record
    rs = [r for r in records(path) if r[1:] == (b"th", b"keepB")]
    if rs:
        seg, tgt = path, rs[-1][0]
        print("keepB at %d in gen %d, the segment ending %s" % (tgt, gen,
            [k.decode("ascii", "replace") for _, _, k in records(path)[-5:]]))
        break
assert seg, cands
with open(seg, "r+b") as f:
    f.seek(tgt + 20)
    b = f.read(1)
    f.seek(tgt + 20); f.write(bytes([b[0] ^ 0xFF]))
EOF
"$BIN" -W "$D/wal" | grep -q "stop=torn" && ok || bad "-W missed the tear"
newlog; start
grep -qE "wal replay [0-9]+ applied" "$D/log" && ok || bad "no replay line"
python3 - <<'EOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
[ $? -eq 0 ] && ok || bad "tear gating ($(cat "$D/tear.txt"); snapshots: $(grep -h "rdb:" "$D"/log* | tail -2 | tr '\n' ' ' | cut -c1-200))"
hardkill

# ---- 5. corrupt snapshot: cold start, WAL-alone rebuild ----------------
rm -rf "$D/wal"; mkconf
newlog; start
python3 - <<'EOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s); rid=[0]
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
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 16487), timeout=5)
f = pcnative.wrap(s)
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
