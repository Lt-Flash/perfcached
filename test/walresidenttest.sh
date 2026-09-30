#!/bin/sh
# walresidenttest.sh - S265: the WAL's buffers are resident from the start.
#
# Each producer thread's WAL ring (ring_kb, 1 MB by default) and the pump's
# 4 MB stage buffer were malloc'd and left untouched.  A block that size is
# its own lazily faulted mapping, so its pages became resident only as the
# ring head first passed them: RSS grew with every byte logged until each
# ring had wrapped once.  On the test fleet that read as a ~0.7 MB/h leak
# after every restart and failed the soak gate's 10% RSS rule - heaptrack
# found no allocation at all, only first touches.
#
# One daemon, 8 MB rings, the arena's give-back off (so RSS is steady
# from the start), then 24 MB of WAL records through one connection -
# 48,000 SETs of 500 bytes to ONE key, so the store does not grow:
#   1. the WAL appended at least 24 MB (the ring wrapped - positive control);
#   2. RSS outside the arena rose by less than 5 MB over it (the fix
#      measures 0-2 MB, the build before it ~10 MB); a sanitizer build
#      skips this - its RSS is the sanitizer's.
# Fail-first: the build before S265 grows by about a ring (8 MB) plus the
# stage buffer.
# Usage: test/walresidenttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwalres.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18681 18682; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "walresidenttest: port $p busy" >&2; exit 1; }
done

mkdir -p "$D/wal"
cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
reclaim_giveback = 0
huge_pages = off
[secrets]
client = wr-client-secret
resp = wr-resp-secret
[listen]
resp = 127.0.0.1:18681
http = 127.0.0.1:18682
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = everysec
ring_kb = 8192
segment_mb = 16
segments = 4
save = off
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "daemon did not start: $(tail -2 "$D/n.log")"; exit 1; }

rss() { awk '/^VmRSS:/ { print $2 }' /proc/$P/status; }
walbytes() { curl -s "http://127.0.0.1:18682/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wal"]["bytes"])' 2>/dev/null || echo 0; }
# the arena's held bytes: only what grew OUTSIDE the arena is the WAL's
held() { curl -s "http://127.0.0.1:18682/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["memory"]["arena_held"] // 1024)' 2>/dev/null || echo 0; }
# A sanitizer build's RSS is not the WAL's to answer for: over the same
# writes ASan maps ~48 allocator regions, fills shadow and pages in its
# larger code (+3.7 MB on 222, +12 MB on a GitHub runner - rc55 red).
SAN=; grep -qa "__asan_init" "$BIN" 2>/dev/null && SAN=asan
# steady first: two readings 2 s apart within 256 kB
prev=$(rss); i=0
while [ $i -lt 15 ]; do
	sleep 2; cur=$(rss)
	[ $((cur - prev)) -lt 256 ] && [ $((prev - cur)) -lt 256 ] && break
	prev=$cur; i=$((i+1))
done
R0=$(rss); B0=$(walbytes); H0=$(held)

REFUSED=$(python3 - <<'PY'

import socket, sys
s = socket.create_connection(("127.0.0.1", 18681), 10); f = s.makefile("rwb")
def cmd(*a):
    return ("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode()
f.write(cmd("AUTH", "wr-resp-secret")); f.flush(); f.readline()
val = "x" * 500
bad = 0
for chunk in range(48):
    for i in range(1000):
        f.write(cmd("SET", "one", val))
    f.flush()
    for i in range(1000):
        if f.readline() != b"+OK\r\n": bad += 1
print(bad)
PY
)
sleep 3                                # the pump drains the ring
R1=$(rss); B1=$(walbytes); H1=$(held)
WB=$(( (B1 - B0) / 1048576 ))
AH=$(( (H1 - H0) / 1024 ))
GROW=$(( (R1 - R0 - (H1 - H0)) / 1024 ))

[ "$WB" -ge 24 ] \
	&& ok "the WAL appended $WB MB - the 8 MB ring wrapped" \
	|| bad "the WAL appended only $WB MB - the ring may not have wrapped, so the next check proves nothing"
# refused writes are backpressure (a slow runner's pump falls behind the
# 8 MB ring), not a fault: what this needs is the bytes appended, above
[ "${REFUSED:-0}" = 0 ] || echo "  note $REFUSED of 48,000 writes refused under backpressure"
if [ -n "$SAN" ]; then
	echo "  SKIP the RSS check: a $SAN build - RSS $((R0 / 1024)) -> $((R1 / 1024)) MB is the sanitizer's, not the WAL's"
else
[ "$GROW" -lt 5 ] \
	&& ok "RSS outside the arena rose $GROW MB over $WB MB of WAL records (RSS $((R0 / 1024)) -> $((R1 / 1024)) MB, arena +$AH MB): the buffers were resident from the start" \
	|| bad "RSS outside the arena rose $GROW MB over $WB MB of WAL records (RSS $((R0 / 1024)) -> $((R1 / 1024)) MB, arena +$AH MB): the WAL buffers were faulted in by use"
fi
echo "walresidenttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
