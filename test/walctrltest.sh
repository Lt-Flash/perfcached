#!/bin/sh
# walctrltest.sh - the recycling rule has to survive a restart.
#
# Both sides of "is this segment free" must be durable: the marker (in
# the rdb) always was, the per-segment spans were not - they lived in a
# calloc'd array, so after a restart every span read zero, seg_hot()
# answered "cold" for every segment, and the rule documented in DESIGN
# section 7 was not enforced at all.  The sequence itself restarted from
# zero too, which is what makes it observable from outside: a log whose
# records are numbered 1..N, then 1..M again, is not ascending.
# Usage: test/walctrltest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d "${PCCTL_TMP:-/var/tmp}/pcctl.XXXXXX")
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkdir -p "$D/wal"
cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = ctl-client-secret
cluster = ctl-cluster-secret
[listen]
tcp = 127.0.0.1:16493
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = always
segment_mb = 1
segments = 4
save = off
[collection th]
buckets_log2 = 12
EOF

start() {
	"$BIN" -f "$D/c.conf" >> "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; tail -5 "$D/log"; exit 1
}
stop() { [ -n "$PID" ] && kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }

write_keys() {   # write_keys <prefix> <count>
	python3 - "$1" "$2" <<'PY'
import json, pcnative, socket, sys
pre, n = sys.argv[1], int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", 16493), timeout=10)
f = pcnative.wrap(s)
f.write(b"".join((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
    {"col":"th","key":"%s%05d"%(pre,i),"value":"v"*700}})+"\n").encode()
    for i in range(n)))
f.flush()
for _ in range(n): json.loads(f.readline())
PY
}

echo "=== walctrltest ==="
: > "$D/log"
start
write_keys a 900
stop

[ -s "$D/wal/CONTROL" ] && ok "CONTROL written" || bad "no CONTROL file"

# the sequence the first incarnation reached
S1=$("$BIN" -W "$D/wal" | grep -c '^seq=')
[ "$S1" -gt 0 ] && ok "first generation logged $S1 records" \
	|| bad "nothing in the WAL"

: > "$D/log"
start
write_keys b 900
stop

# THE assertion: one log, two incarnations, still one ascending sequence
ORD=$("$BIN" -W "$D/wal" | tail -1)
case "$ORD" in
*seq-order=ascending*) ok "sequence continues across the restart" ;;
*) bad "sequence restarted: $ORD" ;;
esac

# the marker the post-recovery checkpoint stamped: 0 means the whole WAL
# gets replayed again on the next start, forever
M=$(grep "wal marker" "$D/log" | tail -1 | sed 's/.*wal marker //;s/[^0-9].*//')
[ -n "$M" ] && [ "$M" -gt 0 ] 2>/dev/null && ok "post-recovery marker is $M" \
	|| bad "post-recovery marker is ${M:-absent} - the replay window never shrinks"

# S438: a clean stop closes the active segment's span - nothing is left open
grep -q "left open by a stop without a shutdown" "$D/log" \
	&& bad "S438: a clean stop left a segment span open" \
	|| ok "S438: a clean stop leaves no segment span open"

# spans came back: the second incarnation must know what the first wrote
G=$("$BIN" -W "$D/wal" | grep -c '^seq=')
[ "$G" -ge "$S1" ] && ok "both generations replay ($G records)" \
	|| bad "lost records across the restart ($S1 -> $G)"

# RED: a torn CONTROL is not fatal, it is conservative
printf '\xff\xff\xff\xff' | dd of="$D/wal/CONTROL" bs=1 seek=16 conv=notrunc 2>/dev/null
: > "$D/log"
start
grep -q "CONTROL is torn" "$D/log" && ok "a torn CONTROL is reported" \
	|| bad "a torn CONTROL passed unnoticed"
stop

# S438: a span left open by a stop WITHOUT a shutdown.  The active
# segment's span is closed only by the next rotation or a clean stop; a
# kill -9 leaves it open in CONTROL, the next start activates the slot
# after it, and nothing closed it again: it read as holding records newer
# than every snapshot, and one lap later its recycle was a FULL overrun
# of records the post-recovery checkpoint covered (245, 2026-10-23).  A
# fresh WAL: ~300 KB into the first segment, kill -9, restart (the replay
# and its checkpoint cover all of it), then writes until the third
# rotation recycles the killed segment, and short of a fourth (which
# would recycle a segment written since, with no snapshot - save = off).
# FAIL-FIRST: the build before S438 logs "wal: FULL - no free segment".
rm -rf "$D/wal"; mkdir -p "$D/wal"
: > "$D/log"
start
write_keys c 400
kill -9 $PID; wait $PID 2>/dev/null; PID=
: > "$D/log"
start
grep -q "left open by a stop without a shutdown" "$D/log" \
	&& ok "S438: the restart closed the span the kill left open: $(grep -o 'wal: [0-9]* segment span.*' "$D/log" | head -1 | cut -c1-110)" \
	|| bad "S438: no open span closed after a kill -9"
# the ring's generation, read off the segment headers (activation writes
# them, so it is what has rotated, whatever is still queued)
maxgen() {
	python3 - "$D/wal" <<'PY'
import glob, struct, sys
g = []
for p in sorted(glob.glob(sys.argv[1] + "/wal-*.seg")):
    with open(p, "rb") as f:
        h = f.read(8)
    g.append(struct.unpack("<II", h)[1] if len(h) == 8 else 0)
print(max(g) if g else 0)
PY
}
# driven to the killed segment's slot, not sized for one machine's speed:
# a fixed 4,800 writes reached generation 4 and 3 on slower runners (ASan,
# fsync = always), 5 on others.  300 a batch, under a quarter segment,
# synced before each look, so nothing queued can reach a sixth -
# which would recycle a segment written since the restart, never covered
# by a snapshot (save = off): a real overrun, not this test's
walsync() {   # the sync verb: back once everything appended is synced
	python3 - <<'PY'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16493), timeout=30))
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "sync", "params": {"timeout_ms": 20000}}) + "\n").encode()); f.flush()
f.readline()
PY
}
n=0
while [ $n -lt 40 ]; do
	write_keys "d$n-" 300
	walsync                # nothing queued behind what maxgen reads
	n=$((n + 1))
	[ "$(maxgen)" -ge 5 ] 2>/dev/null && break
done
# a write is acknowledged before the pump has logged it: a clean stop
# drains every ring (pc_wal_shutdown) before anything is read
stop
G=$(maxgen)
[ "$G" = 5 ] && ok "S438: the ring wrapped onto the killed segment and no further (generation $G, $n batches of 300)" \
	|| bad "S438: the ring reached generation $G after $n batches of 300, want 5"
grep -q "wal: FULL" "$D/log" \
	&& bad "S438: a false overrun - $(grep -o 'wal: FULL.*' "$D/log" | head -1 | cut -c1-160)" \
	|| ok "S438: no overrun when the ring recycled it - its records were covered"

echo "walctrltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
