#!/bin/sh
# pinbothtest.sh - S265: the arena and the WAL buffers are locked in RAM
# together or not at all.
#
# The arena is mlock'd when RLIMIT_MEMLOCK allows (pin = auto) or must be
# (pin = require); the WAL's rings and the pump's stage buffer never were.
# A node could therefore report itself resident while its write path
# stalled on a swapped-out ring page.  Now, when the arena is locked, the
# WAL buffers are locked with it; if they cannot be, `pin = auto` unlocks
# the arena as well and warns, and `pin = require` refuses to start.
#
# Ground truth is the kernel's VmLck.  16 MB arena, 2 workers (8 rings of
# 1 MB + the 4 MB stage = 12 MB of WAL buffers), the soft RLIMIT_MEMLOCK
# set per case (no privilege needed below the hard limit):
#   A. 60 MB, auto:    both locked - VmLck >= 28 MB, wal.buffers_locked
#   B. 20 MB, auto:    the arena fits, the WAL does not - NEITHER locked
#                      (VmLck 0) and the node says why
#   C. 20 MB, require: refuses to start, naming both
#   D. <=8 MB, auto:   the arena cannot lock - neither locked, as before
# A-C need a hard limit of 20-60 MB and are SKIPPED, loudly, below it
# (default containers and CI runners); D runs under any limit.
# Fail-first: the build before S265 locks only the arena in A and B
# (VmLck 16 MB) and starts in C.
# Usage: test/pinbothtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpin.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); echo "  ok   $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL $1"; }
skp()  { skip=$((skip+1)); echo "  SKIP $1"; }
for p in 18701 18702; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "pinbothtest: port $p busy" >&2; exit 1; }
done
HARD=$(ulimit -H -l)
[ "$HARD" = unlimited ] && HARD=1048576
# CAP_IPC_LOCK (bit 14) ignores RLIMIT_MEMLOCK altogether - root on a
# bare host has it, root in a default container and an unprivileged
# runner do not.  With it, the daemon is started without it.
CAPEFF=$(awk '/^CapEff:/ { print $2 }' /proc/self/status)
DROP=
if [ $(( 0x$CAPEFF >> 14 & 1 )) = 1 ]; then
	if command -v setpriv > /dev/null 2>&1; then
		DROP="setpriv --bounding-set=-ipc_lock --inh-caps=-ipc_lock"
	else
		echo "pinbothtest: SKIPPED - CAP_IPC_LOCK held and no setpriv to drop it, so no limit can bind"
		exit 0
	fi
fi

conf() { # conf <pin>
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
huge_pages = off
pin = $1
[secrets]
client = pin-client-secret
[listen]
tcp = 127.0.0.1:18701
http = 127.0.0.1:18702
plaintext = loopback
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = everysec
ring_kb = 1024
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n.conf"
}
# start <memlock kB>: sets P, or returns 1 with the log in $D/n.log
start() {
	: > "$D/n.log"
	$DROP sh -c 'ulimit -S -l "$1" && exec "$2" -f "$3"' sh "$1" "$BIN" "$D/n.conf" >> "$D/n.log" 2>&1 &
	P=$!
	i=0; while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n.log" && return 0
		kill -0 $P 2>/dev/null || { P=; return 1; }
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stop() { [ -n "$P" ] && kill $P 2>/dev/null; [ -n "$P" ] && wait $P 2>/dev/null; P=; }
vmlck() { awk '/^VmLck:/ { print int($2 / 1024) }' /proc/$P/status; }
walflag() { curl -s "http://127.0.0.1:18702/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wal"].get("buffers_locked"))' 2>/dev/null; }

echo "--- A. 60 MB, auto: both"
if [ "$HARD" -lt 61440 ]; then skp "A: hard RLIMIT_MEMLOCK is $HARD kB"; else
	conf auto
	if start 61440; then
		L=$(vmlck); F=$(walflag)
		[ "$L" -ge 28 ] && [ "$F" = True ] \
			&& ok "A: arena and WAL buffers locked - VmLck $L MB, wal.buffers_locked $F" \
			|| bad "A: VmLck $L MB, wal.buffers_locked $F - want >= 28 MB and True"
	else bad "A: did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; fi
	stop
fi

echo "--- B. 20 MB, auto: the arena fits, the WAL does not"
if [ "$HARD" -lt 20480 ]; then skp "B: hard RLIMIT_MEMLOCK is $HARD kB"; else
	conf auto
	if start 20480; then
		L=$(vmlck); F=$(walflag)
		[ "$L" = 0 ] && [ "$F" = False ] \
			&& ok "B: neither locked (VmLck 0, wal.buffers_locked False) - not half pinned" \
			|| bad "B: VmLck $L MB, wal.buffers_locked $F - want 0 and False (half pinned)"
		grep -q "the arena is unlocked too" "$D/n.log" \
			&& ok "B: the node says why: $(grep -o 'the WAL buffers ([0-9]* MB) could not be locked' "$D/n.log" | head -1)" \
			|| bad "B: no warning that the arena was unlocked with the WAL"
	else bad "B: did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; fi
	stop
fi

echo "--- C. 20 MB, require: refuse"
if [ "$HARD" -lt 20480 ]; then skp "C: hard RLIMIT_MEMLOCK is $HARD kB"; else
	conf require
	if start 20480; then
		bad "C: started with pin = require and the WAL buffers unlockable (VmLck $(vmlck) MB)"
		stop
	else
		grep -q "locked together or not at all" "$D/n.log" \
			&& ok "C: refused to start: $(grep -o 'the WAL buffers ([0-9]* MB) could not be' "$D/n.log" | head -1)" \
			|| bad "C: did not start, but not for this: $(tail -2 "$D/n.log" | tr '\n' ' ')"
	fi
fi

# D runs under any hard limit: it needs only one below the arena's 16 MB
LD=8192; [ "$HARD" -lt $LD ] && LD=$HARD
echo "--- D. $LD kB, auto: the arena cannot lock"
if true; then
	conf auto
	if start $LD; then
		L=$(vmlck); F=$(walflag)
		[ "$L" = 0 ] && [ "$F" = False ] \
			&& ok "D: neither locked (VmLck 0, wal.buffers_locked $F)" \
			|| bad "D: VmLck $L MB, wal.buffers_locked $F - want 0 and False"
	else bad "D: did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; fi
	stop
fi
echo "pinbothtest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
