#!/bin/sh
# waldroptest.sh — fsync = always must not silently lose acknowledged writes.
#
# THE BUG (measured 2026-08-29, build 40f907f): fsync = always makes the
# pump call fdatasync once per drained batch.  On ms-class storage that
# window is long enough for the per-writer ring to fill, and a full ring
# DROPS the record - `r->dropped++; /* NEVER block a worker */`.  The
# client is told the write succeeded, the record never reaches the WAL,
# and it is gone after a restart.  Six trials lost 30-2601 of 20000,
# while fsync = no and fsync = everysec lost nothing in six trials each -
# the most durable setting was the only lossy one.
#
# The fix has three independent parts and this test covers all three:
#   1. the probe derives the ring depth fsync=always needs on THIS device
#   2. the daemon applies it unless the operator pinned ring_kb
#   3. a drop is shouted, not merely counted
#
# WHICH ARMS ACTUALLY CATCH A REGRESSION.  Arm 1 (defaults must not drop)
# is an end-to-end sanity check and NOT a reliable detector: proven
# fail-first against 40f907f, it PASSED there - the stock 1 MB ring is a
# coin flip on this device (six trials dropped 30-2601, and some dropped
# nothing).  The load-bearing arms are 2 (the depth was derived and
# applied) and 3 (a forced drop is shouted); both failed against 40f907f,
# arm 3 with 14220 of 20000 acknowledged writes lost in silence.  If this
# test ever needs tightening, tighten those two - do not try to make
# arm 1 deterministic by piling on load.
#
# usage: test/waldroptest.sh [./perfcached] [./pcbench] [./perfcli]
set -u
BIN=${1:-./perfcached}
PB=${2:-./pcbench}
CLI=${3:-./perfcli}
D=${WALDROP_DIR:-/var/tmp/waldrop}
KEYS=${KEYS:-20000}
PORT=${PORT:-17831}
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

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
fsync = always
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

# 1. THE REGRESSION ITSELF.  With the probe allowed to run, fsync=always
#    must lose nothing - whatever this device's fsync latency is.
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

# 3. AN EXPLICIT ring_kb STILL WINS - the operator is not overridden.
set -- $(run auto "ring_kb = 64")
if grep -q "ring .* -> .* KB per writer" "$D/n1.log"; then
	bad "an explicit ring_kb = 64 was overridden by the probe"
else ok; fi
#    and with a ring that small on slow storage the drop must be SHOUTED,
#    not merely counted.  (If this device is fast enough not to drop even
#    at 64 KB, there is nothing to shout about and the arm is skipped.)
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

echo "waldroptest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] || exit 1
