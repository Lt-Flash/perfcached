#!/bin/sh
# snaptriggertest.sh - S224: the WAL asks for a snapshot EARLY enough.
#
# A snapshot frees only the WAL segments below the marker it STARTED at;
# everything written while it runs lands above that marker.  So a WAL
# under write pressure must ask for a snapshot while its free space still
# outlasts one.  The old trigger fired at a fixed quarter free: on a node
# whose snapshot takes longer than that last quarter takes to fill it was
# too late by construction - the ring overruns, and an overrun takes the
# node out of write service (FAILED until restarted; HEALING until a
# snapshot lands since S229).  S224 adds a rate trigger:
# free segments x seconds per segment against twice the last snapshot's
# measured duration.
#
# Asserted, one node, an 8 x 1 MB ring, snapshots throttled to 1 MB/s
# (rdb_mb_s) so one of a ~4 MB table takes about four seconds, the table
# then overwritten at a paced ~0.6 MB/s for 30 s.  The numbers are chosen,
# not found: the quarter fires with 2 segments (~3.3 s) left against a
# snapshot of 4.2 s or more, so it MUST overrun; the rate trigger fires
# near 5 left, a snapshot uses at most ~3, and back-to-back ones need ~6
# of the 8 - so a timely trigger can keep up and a late one cannot:
#   1. `save = 0 N` is refused, and the refusal says why;
#   2. under that load the rate trigger fires (it says so in the log),
#      snapshots keep coming, and the ring NEVER overruns - the node stays
#      writable;
#   3. stats.wal publishes the fill rate and the time to full while it
#      runs, and the rate decays once the writes stop;
#   4. FAIL-FIRST, a step of the run: the same load on a build with the
#      rate trigger short-circuited (the quarter alone) overruns.
# RV-12: the alert S224 documents must be writable from /metrics, not only
# from /stats.  The README's rule, evaluated here on scraped values:
#   perfcached_wal_full_in_seconds >= 0
#     and perfcached_wal_full_in_seconds < 2 * perfcached_rdb_last_duration_seconds
#   5. on the fresh node, before any snapshot: the rdb series exist (0
#      saves, 0 s duration, age -1) and the rule is quiet;
#   6. under the load above it FIRES - sampled every 0.25 s, since it holds
#      only between a trigger and the snapshot that answers it - and after
#      it, the series agree with /stats.
# Usage: test/snaptriggertest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsn.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=17331
HPORT=17332

echo "--- building the variant without the rate trigger, for the fail-first"
mkdir -p "$D/t"
tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
	(cd "$D/t" && tar -xf -) || { echo "cannot copy the tree"; exit 1; }
sed -i 's/if (W.seg_ewma_s > 0) {/if (0 \&\& W.seg_ewma_s > 0) {/' "$D/t/src/wal.c"
grep -q "if (0 && W.seg_ewma_s > 0) {" "$D/t/src/wal.c" ||
	{ echo "the sed missed - the rate trigger is still there"; exit 1; }
make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.log" 2>&1 || {
	echo "the variant did not build:"; tail -5 "$D/build.log"; exit 1; }
cp "$D/t/perfcached" "$D/perfcached.quarter"

conf() { # conf <save-line>
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = sn-client-secret
cluster = sn-cluster-secret
[listen]
tcp = 127.0.0.1:$PORT
http = 127.0.0.1:$HPORT
plaintext = loopback
[collection c]
buckets_log2 = 12
[wal]
dir = $D/wal
probe = no
fsync = no
segment_mb = 1
segments = 8
save = $1
rdb_mb_s = 1
# the fill is 4 MB in quick batches: a 1 MB producer ring overflows,
# DROPS acknowledged writes, and a drop takes the node out of write
# service (S58; S229 heals it) - while it lasts every write is refused
# and this whole test measures nothing.  Seen.
ring_kb = 16384
EOF
	chmod 600 "$D/n.conf"
}
start() { # start <binary>
	: > "$D/n.log"
	"$1" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	echo $! > "$D/n.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n.log" && return 0
		kill -0 "$(cat "$D/n.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; return 1
}
stop() { kill -9 "$(cat "$D/n.pid" 2>/dev/null)" 2>/dev/null; rm -f "$D/n.pid"; sleep 0.3; }
wal() { # wal <field>
	printf '{"jsonrpc":"2.0","id":1,"method":"stats"}\n' | timeout 10 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
s.sendall(sys.stdin.read().encode())
r = json.loads(s.makefile("rb").readline())["result"]
w = r.get("wal") or {}; d = r.get("rdb") or {}
v = w.get(sys.argv[2], d.get(sys.argv[2], "?"))
print(v)' $PORT "$1" 2>/dev/null || echo "?"; }
# the documented rule over one /metrics scrape: prints
# "<fires 0|1> saves=.. dur=.. age=.. inprog=.. full_in=.." or "MISSING <names>"
ALERT_PY='
import sys, urllib.request
def scrape(port):
    m = {}
    for ln in urllib.request.urlopen("http://127.0.0.1:%d/metrics" % port, timeout=5).read().decode().splitlines():
        if ln and ln[0] != "#":
            k, _, v = ln.rpartition(" ")
            m[k] = float(v)
    return m
NEED = ("perfcached_wal_full_in_seconds", "perfcached_rdb_last_duration_seconds",
        "perfcached_rdb_saves_total", "perfcached_rdb_last_success_age_seconds",
        "perfcached_rdb_in_progress")
def verdict(m):
    miss = [n for n in NEED if n not in m]
    if miss:
        return "MISSING " + " ".join(miss)
    fi, d = m["perfcached_wal_full_in_seconds"], m["perfcached_rdb_last_duration_seconds"]
    fires = fi >= 0 and fi < 2 * d
    return "%d saves=%g dur=%g age=%g inprog=%g full_in=%g" % (fires,
        m["perfcached_rdb_saves_total"], d, m["perfcached_rdb_last_success_age_seconds"],
        m["perfcached_rdb_in_progress"], fi)
'
alert() { timeout 10 python3 -c "$ALERT_PY
print(verdict(scrape(int(sys.argv[1]))))" $HPORT 2>/dev/null || echo "NO-SCRAPE"; }
# sampler <seconds>: the rule every 0.25 s; prints how often it fired,
# how often a snapshot was in progress, and the samples taken
sampler() { timeout $(($1 + 20)) python3 -c "$ALERT_PY
import time
end = time.time() + float(sys.argv[2]); n = fired = inprog = 0; miss = \"\"
while time.time() < end:
    try:
        v = verdict(scrape(int(sys.argv[1])))
    except Exception:
        v = \"\"
    if v.startswith(\"MISSING\"):
        miss = v
    elif v:
        n += 1; fired += v[0] == \"1\"; inprog += \" inprog=1 \" in v
    time.sleep(0.25)
print(\"samples %d fired %d inprog %d %s\" % (n, fired, inprog, miss))" $HPORT "$1"; }

# load <seconds>: fill 4000 x 1 KB, snapshot it (the measured duration the
# trigger needs), then overwrite the same keys paced at ~0.6 MB/s
load() { timeout 120 python3 -c '
import json, socket, sys, time
port, secs = int(sys.argv[1]), float(sys.argv[2])
f = socket.create_connection(("127.0.0.1", port), timeout=30).makefile("rwb")
v = "x" * 1000
refused = 0
def batch(start, n):
    global refused
    for i in range(start, start + n):
        k = "k%05d" % (i % 4000)
        f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":{"col":"c","key":k,"value":v}})+"\n").encode())
    f.flush()
    for _ in range(n):
        # a reply is not a write: count the ones that were not stored,
        # or a node that failed half way looks like a node under load
        if not (json.loads(f.readline()).get("result") or {}).get("stored"):
            refused += 1
# the fill in batches of 200, each read back before the next: one 4 MB
# burst written before any reply is read deadlocks once the unread
# replies of the daemon fill the socket buffers - it stops reading, the
# writer blocks.  Loopback buffers here held the burst; those of the
# GitHub runner did not (rc32 check red: seq stuck at 2463 of 4000).
for s0 in range(0, 4000, 200):
    batch(s0, 200)
f.write(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"save\"}\n"); f.flush(); f.readline()
time.sleep(6)                       # let that snapshot finish and be timed
t0 = time.time(); i = 0; refused = 0
while time.time() - t0 < secs:
    tick = time.time()
    batch(i, 60); i += 60             # ~60 KB
    rest = 0.1 - (time.time() - tick)
    if rest > 0: time.sleep(rest)     # ~0.6 MB/s
print("writes %d refused %d" % (i, refused))' $PORT "$1"; }

# ---- 1. the config refusal ----------------------------------------------
echo "--- 1. a changes-only rule is refused, with the reason"
conf "0 10000"
OUT=$("$BIN" -C -f "$D/n.conf" 2>&1)
echo "$OUT" | grep -q "seconds must be at least 1" && echo "$OUT" | grep -q '"1 10000"' \
	&& ok "save = 0 10000 is refused, and says why and what to write instead" \
	|| bad "the refusal did not explain itself: $(echo "$OUT" | tail -1)"
conf "1 10000"
"$BIN" -C -f "$D/n.conf" >/dev/null 2>&1 && ok "save = 1 10000 is accepted" || bad "save = 1 10000 refused"

# ---- 2 + 3. the rate trigger under load ---------------------------------
echo "--- 2. an 8 MB ring under ~0.6 MB/s against a ~4 s snapshot"
conf off
start "$BIN" || { bad "did not start"; echo "snaptriggertest: $pass passed, $fail failed"; exit 1; }
# ---- 5. RV-12: the rule's series, before any snapshot -------------------
A=$(alert)
echo "    fresh node: $A"
case "$A" in
	"0 saves=0 dur=0 age=-1 inprog=0 "*)
		ok "RV-12: before any snapshot /metrics has the rdb series (0 saves, 0 s, age -1) and the rule is quiet";;
	*)	bad "RV-12: the rule's series on a fresh node: $A";;
esac
load 30 > "$D/load.out" 2>&1 &
LP=$!
sampler 40 > "$D/sampler.out" 2>&1 &
SP=$!
# a timeline of the WAL's own counters, so a failure here says WHERE
FILL=0; FULL=-1
for t in 5 10 15 20 25 30 35; do
	sleep 5
	L=$(wal last_seq); R=$(wal recycles); FS=$(wal free_segments); FM=$(wal fill_mb_s); FI=$(wal full_in_s)
	echo "    t=${t}s seq=$L recycles=$R free=$FS fill=${FM}MB/s full_in=${FI}s"
	case "$FM" in ''|'?'|0|0.0) ;; *) FILL=$FM; FULL=$FI;; esac
done
wait $LP
echo "    driver: $(tail -2 "$D/load.out" | tr '\n' ' ')"
grep -q "refused 0$" "$D/load.out" && [ "$(wal dropped)" = 0 ] \
	&& ok "every write of the load was stored and logged (0 refused, 0 dropped) - the load is real" \
	|| bad "the load did not land: $(tail -1 "$D/load.out"), wal dropped $(wal dropped)"
O=$(wal overruns); SAVES=$(wal saves); DUR=$(wal last_dur_ms)
echo "    rate trigger: overruns $O, snapshots $SAVES (last $DUR ms); mid-load fill $FILL MB/s, full in $FULL s"
grep -q "filling faster than a snapshot can keep up" "$D/n.log" \
	&& ok "the rate trigger fired: $(grep -m1 "filling faster" "$D/n.log" | sed 's/.*wal: //' | cut -c1-90)..." \
	|| bad "the rate trigger never fired"
[ "$O" = 0 ] && ok "and the ring never overran: $SAVES snapshots kept up, the node stayed writable" \
	|| bad "the ring overran $O time(s) - the node is FAILED"
case "$FILL" in ''|'?'|0|0.0) bad "no fill rate published mid-load ($FILL)";;
	*) ok "stats.wal published it while it ran: filling at $FILL MB/s, full in $FULL s";; esac
# ---- 6. RV-12: the rule fired under the load, and agrees with /stats ----
wait $SP
echo "    sampler: $(cat "$D/sampler.out")"
set -- $(cat "$D/sampler.out")
[ "${2:-0}" -gt 0 ] 2>/dev/null && [ "${4:-0}" -gt 0 ] 2>/dev/null && [ -z "${7:-}" ] \
	&& ok "RV-12: the documented rule FIRED under the load ($4 of $2 samples), from /metrics alone" \
	|| bad "RV-12: the rule never fired from /metrics: $(cat "$D/sampler.out")"
[ "${6:-0}" -gt 0 ] 2>/dev/null \
	&& ok "RV-12: perfcached_rdb_in_progress showed the snapshots running ($6 samples)" \
	|| bad "RV-12: in_progress never read 1 across $SAVES snapshots"
# /stats and /metrics read back to back, bracketed by a second /stats: a
# snapshot finishing between the reads (they keep coming for a few seconds
# after the load) moves both, and is a retry, not a disagreement.  Seen:
# the first cut compared against a /stats read five seconds stale.
i=0
while [ $i -lt 5 ]; do
	S1=$(wal saves); D1=$(wal last_dur_ms); A=$(alert); S2=$(wal saves)
	[ "$S1" = "$S2" ] && [ "$(wal running)" = False ] && break
	sleep 2; i=$((i+1))
done
DURS=$(python3 -c "print('%g' % (int('$D1') / 1000))" 2>/dev/null)
echo "    after: $A (stats: $S1 saves, $D1 ms)"
case "$A" in
	?" saves=$S1 dur=$DURS age="*) AGE=$(echo "$A" | sed 's/.* age=\([^ ]*\) .*/\1/')
		[ "$AGE" -ge 0 ] 2>/dev/null && [ "$AGE" -lt 60 ] 2>/dev/null \
			&& ok "RV-12: after it, /metrics agrees with /stats ($S1 saves, ${DURS} s) and the age is real (${AGE} s)" \
			|| bad "RV-12: the snapshot age reads $AGE";;
	*)	bad "RV-12: /metrics disagrees with /stats after the load: $A";;
esac
sleep 6
IDLE=$(wal fill_mb_s)
python3 -c "import sys; sys.exit(0 if float('$IDLE') < float('$FILL') else 1)" 2>/dev/null \
	&& ok "and the rate decays once the writes stop ($FILL -> $IDLE MB/s)" \
	|| bad "the rate did not decay when idle ($FILL -> $IDLE)"
stop

# ---- 4. fail-first --------------------------------------------------------
echo "--- 4. fail-first: the same load, the quarter trigger alone"
conf off
start "$D/perfcached.quarter" || bad "the variant did not start"
load 30 > "$D/load2.out" 2>&1
echo "    driver: $(tail -2 "$D/load2.out" | tr '\n' ' ')"
O=$(wal overruns); SAVES=$(wal saves)
echo "    quarter only: overruns $O, snapshots $SAVES"
case "$O" in ''|'?'|0) bad "the quarter trigger alone did not overrun either - this load proves nothing";;
	*) ok "the quarter alone overruns ($O) - the node leaves write service - which is what the rate trigger prevents";; esac
stop

echo "snaptriggertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
