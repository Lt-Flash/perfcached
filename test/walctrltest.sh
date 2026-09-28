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

BIN=${1:-./perfcached}
D=$(mktemp -d "${PCCTL_TMP:-/var/tmp}/pcctl.XXXXXX")
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
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
import json, socket, sys
pre, n = sys.argv[1], int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", 16493), timeout=10)
f = s.makefile("rwb")
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

echo "walctrltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
