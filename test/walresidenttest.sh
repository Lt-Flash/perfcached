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
# from the start), then ~33 MB of WAL records through one connection -
# 64,000 SETs of 500 bytes to ONE key, so the store does not grow:
#   1. the WAL appended at least 24 MB (the ring wrapped three times -
#      positive control), read once the pump has drained (its appended
#      count stops moving) rather than after a fixed wait: GitHub's 0.4.0
#      run read 23 MB of the old 48,000 writes 3 s after the last, with
#      none refused - the pump had not finished, and the margin was 8%;
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
ring_kb = ${WALRES_RING_KB:-8192}
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
# walstat <key>: one counter of the wal block (staged = records acked and
# not yet appended; dropped = acked records a full ring discarded; heals)
walstat() { curl -s "http://127.0.0.1:18682/stats" | python3 -c 'import json,sys; v = json.load(sys.stdin)["wal"].get(sys.argv[1], 0); print(int(v) if not isinstance(v, bool) else int(v))' "$1" 2>/dev/null || echo 0; }
# the arena's held bytes: only what grew OUTSIDE the arena is the WAL's
held() { curl -s "http://127.0.0.1:18682/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["memory"]["arena_held"] // 1024)' 2>/dev/null || echo 0; }
# A sanitizer build's RSS is not the WAL's to answer for: over the same
# writes ASan maps ~48 allocator regions, fills shadow and pages in its
# larger code (+3.7 MB on 222, +12 MB on a GitHub runner - rc55 red).
SAN=; grep -qa "__asan_init" "$BIN" 2>/dev/null && SAN=asan
# a sanitizer build's pump is 3-5x slower (walring_copy, crc32c, pwrite
# are all instrumented) while the loader is not: every wait below is
# three times longer there
SLOW=1; [ -n "$SAN" ] && SLOW=3; export SLOW
# steady first: two readings 2 s apart within 256 kB
prev=$(rss); i=0
while [ $i -lt 15 ]; do
	sleep 2; cur=$(rss)
	[ $((cur - prev)) -lt 256 ] && [ $((prev - cur)) -lt 256 ] && break
	prev=$cur; i=$((i+1))
done
R0=$(rss); B0=$(walbytes); H0=$(held)

REFUSED=$(python3 - <<'PY'

import json, os, socket, sys, time, urllib.request
SLOW = int(os.environ.get("SLOW", "1"))
s = socket.create_connection(("127.0.0.1", 18681), 10); f = s.makefile("rwb")
def cmd(*a):
    return ("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode()
def staged():
    try:
        return int(json.load(urllib.request.urlopen("http://127.0.0.1:18682/stats", timeout=5))["wal"].get("staged", 0))
    except Exception:
        return 0
f.write(cmd("AUTH", "wr-resp-secret")); f.flush(); f.readline()
val = "x" * 500
bad = 0
for chunk in range(64):
    for i in range(1000):
        f.write(cmd("SET", "one", val))
    f.flush()
    for i in range(1000):
        if f.readline() != b"+OK\r\n": bad += 1
    # pace on the ring gauge, not on luck: a chunk is ~550 KB of records
    # and the ring 8 MB, so hold the next chunk until the pump has fewer
    # than ~4,000 records (a quarter ring) still staged.  The ring then
    # wraps four times at ANY pump speed, and nothing is ever dropped -
    # which is what the next check asserts.  10-04: a GitHub ASan runner
    # read 23 MB with 8,970 refused: the unpaced burst filled the ring,
    # the full ring DROPPED acked records, and the S229 heal gate refused
    # the rest - refusals are the gate, drops are the loss.
    t = time.time()
    while staged() > 4000 and time.time() - t < 10 * SLOW:
        time.sleep(0.05)
print(bad)
PY
)
# the pump drains the ring: wait until nothing is staged (two reads of 0,
# 0.5 s apart) - two equal byte counts cannot tell "drained" from "inside
# an everysec fdatasync", which is how 0.4.0's run read 23 MB with none
# refused.  At most 15 s, 45 under a sanitizer.
zero=0; i=0
while [ $i -lt $((30 * SLOW)) ]; do
	if [ "$(walstat staged)" = 0 ]; then zero=$((zero+1)); [ $zero -ge 2 ] && break; else zero=0; fi
	sleep 0.5; i=$((i+1))
done
R1=$(rss); B1=$(walbytes); H1=$(held)
WB=$(( (B1 - B0) / 1048576 ))
AH=$(( (H1 - H0) / 1024 ))
GROW=$(( (R1 - R0 - (H1 - H0)) / 1024 ))

DROPPED=$(walstat dropped); HEALS=$(walstat heals)
# the positive control must be honest: a record a full ring dropped was
# acked and never reached wal.bytes, and after the first drop the S229
# heal gate refuses every write - neither is "backpressure", both mean
# the run cannot prove residency.  Loud, with the numbers.
[ "$DROPPED" = 0 ] && [ "$HEALS" = 0 ] \
	&& ok "no acked record dropped and no heal: the paced load kept inside the ring" \
	|| bad "the ring filled: $DROPPED acked writes dropped, $HEALS heal(s), ${REFUSED:-0} refused by the heal gate - this run proves nothing about residency"
[ "$WB" -ge 24 ] \
	&& ok "the WAL appended $WB MB - the 8 MB ring wrapped" \
	|| { bad "the WAL appended only $WB MB - the ring may not have wrapped, so the next check proves nothing"
	     echo "    wal: dropped=$DROPPED heals=$HEALS staged=$(walstat staged) refused=${REFUSED:-0}"
	     grep -aE 'DROPPED|HEALING|healed|FAILED|dropped' "$D/n.log" | cut -c1-160 | head -5 | sed 's/^/    n: /'; }
[ "${REFUSED:-0}" = 0 ] || echo "  note $REFUSED of 64,000 writes refused (the S229 heal gate after a ring drop - see the first check)"
if [ -n "$SAN" ]; then
	echo "  SKIP the RSS check: a $SAN build - RSS $((R0 / 1024)) -> $((R1 / 1024)) MB is the sanitizer's, not the WAL's"
else
[ "$GROW" -lt 5 ] \
	&& ok "RSS outside the arena rose $GROW MB over $WB MB of WAL records (RSS $((R0 / 1024)) -> $((R1 / 1024)) MB, arena +$AH MB): the buffers were resident from the start" \
	|| bad "RSS outside the arena rose $GROW MB over $WB MB of WAL records (RSS $((R0 / 1024)) -> $((R1 / 1024)) MB, arena +$AH MB): the WAL buffers were faulted in by use"
fi
echo "walresidenttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
