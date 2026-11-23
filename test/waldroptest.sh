#!/bin/sh
# waldroptest.sh - the WAL ring under slow fsync must not silently lose acknowledged writes: the probe
# sizes it, ring_kb wins, the receipt owns its gaps, and a full ring waits under always and everysec.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
PB=${2:-./pcbench}
CLI=${3:-./perfcli}
SHIM=${4:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "waldroptest: $SHIM is missing (make syncfailshim.so)"; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
# a dynamic sanitizer runtime comes first in the preload list (healtest)
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
# per-run by default (a fixed default collides under concurrent CI jobs); the override lets a human
# keep the tree
D=${WALDROP_DIR:-$(mktemp -d /var/tmp/waldrop.XXXXXX)}
KEYS=${KEYS:-20000}
PORT=${PORT:-17831}
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

# the per-run tree (a 65 MB provisioned WAL each run) is removed on exit; a tree the caller named
# (WALDROP_DIR) is the caller's to keep
[ -n "${WALDROP_DIR:-}" ] || trap 'rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
rm -rf "$D"; mkdir -p "$D/wal"
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
[ -z "$REV" ] || [ "$REV" = unknown ] && { echo "waldroptest: build cannot name itself"; exit 2; }

mkcfg() { # mkcfg <probe> <extra>
	cat > "$D/n1.conf" <<CFG
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = wd-secret
cluster = wd-cluster
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[wal]
dir = $D/wal
segment_mb = 8
probe = $1
fsync = ${FSYNC:-always}
save = off
$2
[collection c]
buckets_log2 = 17
CFG
}
ent() { "$CLI" -h 127.0.0.1 -p "$PORT" -j '{"method":"stats"}' 2>/dev/null | python3 -c "import json,sys
try:
    d=json.load(sys.stdin); w=d['wal']
    print(w['appended'], w['dropped'], d['collections'][0]['entries'])
except Exception: print('-1 -1 -1')"; }

run() { # run <probe> <extra> -> echoes "appended dropped entries"
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	mkcfg "$1" "$2"
	"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
	P=$!
	k=0
	while [ $k -lt 400 ]; do
		grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && break
		sleep 0.2; k=$((k+1))
	done
	"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n "$KEYS" -v 200 \
		>/dev/null 2>&1
	sleep 3
	R=$(ent)
	kill -9 $P 2>/dev/null; wait 2>/dev/null
	echo "$R"
}

echo "=== waldroptest ($REV, $KEYS keys) ==="

# 1. with the probe allowed to run, fsync=always loses nothing - a sanity check, not a reliable
#    detector (the stock ring is a coin flip here): tighten the later arms, not this one
set -- $(run auto "")
echo "  probed:      appended=$1 dropped=$2 entries=$3"
if [ "$3" = "$KEYS" ]; then ok; else bad "fill did not accept $KEYS (entries=$3)"; fi
if [ "$2" = 0 ]; then ok; else bad "fsync=always DROPPED $2 acknowledged writes"; fi
if [ "$1" = "$KEYS" ]; then ok
else bad "only $1 of $KEYS reached the WAL"; fi

# 2. THE DEPTH CAME FROM THE PROBE, and is said out loud.  Without this
#    the test above could pass by luck on fast storage.
if grep -q "ring .* -> .* KB per writer" "$D/n1.log"; then
	ok
	echo "  sized:      $(grep -o 'ring [0-9]* -> [0-9]* KB per writer' "$D/n1.log" | head -1)"
else
	# on NVMe-class storage no bump is needed and none is logged; only
	# a bump that was NEEDED and skipped is a failure
	P99=$(sed -n 's/.*p99 \([0-9]*\)us.*/\1/p' "$D/n1.log" | head -1)
	if [ -n "$P99" ] && [ "$P99" -gt 300 ]; then
		bad "p99 ${P99}us needs a deeper ring and none was applied"
	else
		ok
		echo "  sized:      not needed (p99 ${P99:-?}us is NVMe-class)"
	fi
fi

# 3. AN EXPLICIT ring_kb STILL WINS - the configured value is not overridden.
set -- $(run auto "ring_kb = 64")
if grep -q "ring .* -> .* KB per writer" "$D/n1.log"; then
	bad "an explicit ring_kb = 64 was overridden by the probe"
else ok; fi
#    and with a ring that small a drop would be SHOUTED, but fsync = always now waits instead, so
#    this is n/a (healtest shouts under everysec)
echo "  pinned:      appended=$1 dropped=$2 entries=$3"
if [ "$2" -gt 0 ] 2>/dev/null; then
	if grep -q "DROPPED .* acknowledged write" "$D/n1.log"; then
		ok; echo "  shouted:    yes ($2 dropped)"
	else
		bad "$2 writes were dropped and NOTHING was logged"
	fi
else
	ok; echo "  shouted:    n/a (nothing dropped even at ring_kb = 64)"
fi

# 4. THE BARRIER'S RECEIPT MUST OWN ITS GAPS: a ring-full drop discards a record that already holds
#    a sequence number, so the reply carries the drop count to tell a full barrier from a partial one
rm -rf "$D/wal"; mkdir -p "$D/wal"
mkcfg auto ""
"$BIN" -f "$D/n1.conf" > "$D/n4.log" 2>&1 &
P4=$!
k=0
while [ $k -lt 400 ]; do
	grep -q "perfcached ready" "$D/n4.log" 2>/dev/null && break
	sleep 0.2; k=$((k+1))
done
SR=$(PORT="$PORT" python3 - <<'PY'
import json, os, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(os.environ["PORT"])),
	timeout=20))
rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write((json.dumps(r)+"\n").encode()); f.flush()
    return json.loads(f.readline())
call("set", col="c", key="s58", value="x")
r = call("sync").get("result", {})
print("dropped" if "dropped" in r else "MISSING")
PY
)
kill -9 $P4 2>/dev/null; wait 2>/dev/null
if [ "$SR" = dropped ]; then
	ok; echo "  receipt:    sync carries its drop count"
else
	bad "the sync receipt has no drop count ($SR) - a caller cannot
	     tell a full barrier from one covering survivors only"
fi

# 5. UNDER fsync = always A FULL RING WAITS FOR THE PUMP: the shim delays every WAL write 100 ms;
#    nothing may drop, all must reach the WAL, and the waits must be counted
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/ctl"; : > "$D/shim.log"
mkcfg no "ring_kb = 1024"
PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim.log" LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" \
	"$BIN" -f "$D/n1.conf" > "$D/n5.log" 2>&1 &
P5=$!
k=0
while [ $k -lt 400 ]; do
	grep -q "perfcached ready" "$D/n5.log" 2>/dev/null && break
	sleep 0.2; k=$((k+1))
done
echo walslow > "$D/ctl"
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n "$KEYS" -v 200 >/dev/null 2>&1
ent5() { "$CLI" -h 127.0.0.1 -p "$PORT" -j '{"method":"stats"}' 2>/dev/null | python3 -c "import json,sys
try:
    d=json.load(sys.stdin); w=d['wal']
    print(w['appended'], w['dropped'], d['collections'][0]['entries'], w.get('ring_waits', 0) + w.get('writer_pauses', 0))
except Exception: print('-1 -1 -1 -1')"; }
k=0
while [ $k -lt 60 ]; do
	set -- $(ent5)
	[ "$1" -ge "$KEYS" ] 2>/dev/null && break
	sleep 0.5; k=$((k+1))
done
kill -9 $P5 2>/dev/null; wait 2>/dev/null
echo "  slowed:      appended=$1 dropped=$2 entries=$3 waits+pauses=$4"
grep -q walslow "$D/shim.log" && ok || bad "5. the shim never slowed a WAL write - the arm proves nothing"
[ "$2" = 0 ] && [ "$1" = "$KEYS" ] && [ "$3" = "$KEYS" ] && ok \
	|| bad "5. fsync = always on a slowed device: appended $1, dropped $2, entries $3 of $KEYS (want all, none dropped)"
[ "$4" -gt 0 ] 2>/dev/null && ok || bad "5. waits+pauses $4: no writer ever waited (want > 0 - the ring must have filled)"

# 6. UNDER fsync = everysec A FULL RING DOES NOT DROP EITHER: on the same slowed device the writer
#    waits, its connection held (writer_pauses) or its write in the ring (ring_waits)
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/ctl"; : > "$D/shim.log"
FSYNC=everysec mkcfg no "ring_kb = 64"
PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim.log" LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" \
	"$BIN" -f "$D/n1.conf" > "$D/n6.log" 2>&1 &
P6=$!
k=0
while [ $k -lt 400 ]; do
	grep -q "perfcached ready" "$D/n6.log" 2>/dev/null && break
	sleep 0.2; k=$((k+1))
done
echo walslow > "$D/ctl"
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n "$KEYS" -v 200 >/dev/null 2>&1
ent6() { "$CLI" -h 127.0.0.1 -p "$PORT" -j '{"method":"stats"}' 2>/dev/null | python3 -c "import json,sys
try:
    d=json.load(sys.stdin); w=d['wal']
    print(w['appended'], w['dropped'], d['collections'][0]['entries'], w.get('ring_waits', 0) + w.get('writer_pauses', 0), w.get('heals', -1))
except Exception: print('-1 -1 -1 -1 -1')"; }
k=0
while [ $k -lt 120 ]; do
	set -- $(ent6)
	[ "$1" -ge "$KEYS" ] 2>/dev/null && break
	sleep 0.5; k=$((k+1))
done
kill -9 $P6 2>/dev/null; wait 2>/dev/null
echo "  everysec:    appended=$1 dropped=$2 entries=$3 waits+pauses=$4 heals=$5"
grep -q walslow "$D/shim.log" && ok || bad "6. the shim never slowed a WAL write - the arm proves nothing"
[ "$2" = 0 ] && [ "$1" = "$KEYS" ] && [ "$3" = "$KEYS" ] && [ "$5" = 0 ] && ok \
	|| bad "6. fsync = everysec on a slowed device: appended $1, dropped $2, entries $3 of $KEYS, heals $5 (want all, none dropped, no heal)"
[ "$4" -gt 0 ] 2>/dev/null && ok || bad "6. waits+pauses $4: no writer ever waited (want > 0 - the ring must have filled)"

echo "waldroptest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] || exit 1
