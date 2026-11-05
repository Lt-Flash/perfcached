#!/bin/sh
# walgrowtest.sh - S466 step 2: the WAL's segments follow the load, between
# [wal] segments (the floor, always kept) and segments_max (the cap).
#
# A ring sized for an ordinary hour fills during a burst, and before this
# a full ring was a wait for a snapshot (S466 step 1) at best.  Now the
# log grows in the background when it is half full - or when its free
# segments would last under three snapshots - and gives the extra segments
# back after a quiet spell.  Measured on 207 (4 x 1 MB, a 40,000 x 1 KB
# fill): a fixed ring took 2.77 s and waited 2,684 ms for snapshots; one
# allowed to grow to 24 took 0.77 s, waited 674 ms, and was back at 4
# segments 48 s after the load stopped.
#   1. segments = 4, segments_max = 24: the fill grows the log, nothing is
#      overwritten or healed, every key is stored;
#   2. kill -9 and a restart: the grown log is kept (CONTROL's count) and
#      every key is back;
#   3. a quiet spell (PERFCACHED_WAL_SHRINK_S shortens it): the log is back
#      at the floor and the extra segment files are gone;
#   4. the control - no segments_max: the same fill leaves the log at 4.
# usage: test/walgrowtest.sh [./perfcached] [./pcbench] [./perfcli]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
PB=${2:-./pcbench}
CLI=${3:-./perfcli}
PORT=${PORT:-17873}
D=$(mktemp -d /var/tmp/pcwalgrow.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
[ -z "$REV" ] || [ "$REV" = unknown ] && { echo "walgrowtest: build cannot name itself"; exit 2; }
ss -ltn 2>/dev/null | grep -q ":$PORT[[:space:]]" && { echo "walgrowtest: port $PORT already bound" >&2; exit 1; }

mkcfg() { # mkcfg <extra>
	cat > "$D/n.conf" <<CFG
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = wg-secret
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
segments = 4
segment_mb = 1
save = 5 1
$1
[collection c]
buckets_log2 = 17
CFG
	chmod 640 "$D/n.conf"
}
start() { # start <ready lines expected>
	PERFCACHED_WAL_SHRINK_S=2 "$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	P=$!
	k=0
	while [ $k -lt 300 ]; do
		[ "$(grep -c "perfcached ready" "$D/n.log")" -ge "$1" ] && return 0
		sleep 0.1; k=$((k+1))
	done
	echo "  the node did not start:"; tail -3 "$D/n.log"; return 1
}
wal() { "$CLI" -h 127.0.0.1 -p "$PORT" -j '{"method":"stats"}' 2>/dev/null | python3 -c "import json,sys
try:
    d=json.load(sys.stdin); w=d['wal']
    print(w['segments'], w.get('segments_grown', -1), w.get('segments_retired', -1),
          w['overruns'], w['heals'], d['collections'][0]['entries'])
except Exception: print('-1 -1 -1 -1 -1 -1')"; }
files() { ls "$D/wal" | grep -c '^wal-.*\.seg$'; }

echo "=== walgrowtest ($REV) ==="

# 1. the fill grows the log
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/n.log"
mkcfg "segments_max = 24"
start 1 || exit 1
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n 40000 -v 1024 -c 4 -d 32 >/dev/null 2>&1
sleep 1
set -- $(wal)
GROWN_TO=$1
echo "  grow:     segments=$1 grown=$2 overruns=$4 heals=$5 entries=$6 files=$(files)"
[ "$1" -gt 4 ] 2>/dev/null && [ "$2" -gt 0 ] 2>/dev/null && grep -q "wal: grew to" "$D/n.log" \
	&& ok "1. the log grew under the fill ($1 segments, $2 added, said)" \
	|| bad "1. the log did not grow: $1 segments, $2 added, grew lines $(grep -c 'wal: grew' "$D/n.log")"
[ "$4" = 0 ] && [ "$5" = 0 ] && [ "$6" = 40000 ] && ok "1. nothing overwritten or healed, 40000 stored" \
	|| bad "1. overruns $4, heals $5, entries $6 of 40000"
[ "$(files)" = "$1" ] && ok "1. a segment file per slot ($(files))" || bad "1. $(files) segment files for $1 slots"

# 2. the grown log survives a kill -9
kill -9 $P 2>/dev/null; wait $P 2>/dev/null; P=
start 2 || exit 1
set -- $(wal)
[ "$1" = "$GROWN_TO" ] && [ "$6" = 40000 ] && ok "2. after kill -9 and a restart: $1 segments, 40000 keys" \
	|| bad "2. after the restart: $1 segments (was $GROWN_TO), $6 keys of 40000"

# 3. a quiet spell gives the extra segments back
k=0
while [ $k -lt 90 ]; do
	set -- $(wal)
	[ "$1" = 4 ] && break
	sleep 2; k=$((k+1))
done
echo "  quiet:    segments=$1 retired=$3 entries=$6 files=$(files) after $((k * 2)) s"
[ "$1" = 4 ] && [ "$3" -gt 0 ] 2>/dev/null && grep -q "wal: retired a segment" "$D/n.log" \
	&& ok "3. back at the floor ($3 retired, said)" \
	|| bad "3. after a quiet spell: $1 segments, $3 retired (want 4, more than 0)"
[ "$(files)" = 4 ] && [ "$6" = 40000 ] && ok "3. the extra files are gone and every key is there" \
	|| bad "3. $(files) segment files, $6 keys of 40000"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

# 4. the control: no cap, no growth
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/n.log"
mkcfg ""
start 1 || exit 1
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n 40000 -v 1024 -c 4 -d 32 >/dev/null 2>&1
sleep 1
set -- $(wal)
[ "$1" = 4 ] && [ "$6" = 40000 ] && ok "4. without segments_max the log stays at 4 (40000 stored)" \
	|| bad "4. without segments_max: $1 segments, $6 keys"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

echo "walgrowtest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
