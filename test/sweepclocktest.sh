#!/bin/sh
# sweepclocktest.sh - S212 step 1: the two full-keyspace walks carry a
# clock, and the clock reads what the walk cost.
#
# The eager repair sweep walks the whole table every cycle (sending only
# what the watermark says is owed), and the post-restart reconcile probes
# every key this node replayed, 64 a second.  Both are cheap at today's
# keyspace and O(N) in it, and nothing said what either cost.  Now
# `stats.cluster` (and /metrics, and the replication card) carry:
#   repl_sweep_ms / _scanned / _sent / _cycles - the last COMPLETED
#       repair cycle (one peer, one collection, cursor 0 to 0);
#   reconcile_ms / reconcile_pending - the last completed reconcile pass,
#       first probe to last, and probes issued but not yet answered.
#
#  sweep     - A alone holds 1100 x 4 KB (4.4 MB, more than one 4 MB
#              sweep slice); B starts cold, so A's backfill walk to it
#              takes TWO slices, one per 10 s tick: the completed cycle
#              reads >= 9 s, >= 1100 scanned, >= 1100 sent, 1+ cycles.
#              (A sweep that sends nothing finishes in one slice and
#              reads ~0 ms, which is the truth about it.)
#  reconcile - A restarts with its WAL: it replays the 1100 keys and
#              probes every one of them at 64/s (a slice cut mid-bucket
#              re-probes that bucket, so "probed" is issues, >= 1100);
#              the completed pass reads ~17 s with 0 pending - the pending
#              gauge itself is pinned in clpendtest, because on loopback
#              a probe is answered within the sampling interval.
# Fail-first: a daemon before S212 has no such keys - every read is
# "absent".
# Usage: test/sweepclocktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsw.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
N=1100

conf() { # <node> <wal-dir or ->
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = sw-client-secret
cluster = sw-cluster-secret
[listen]
tcp = 127.0.73.$1:1849$1
plaintext = loopback
[cluster]
multicast = 239.255.77.251:17451
advertise = 127.0.73.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 12
EOF
	[ "$2" != - ] && cat >> "$D/n$1.conf" <<EOF
[wal]
dir = $2
probe = no
fsync = no
segment_mb = 8
segments = 4
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # <node>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
import json, socket, sys, time
def call(n, req):
    f = socket.create_connection(("127.0.73.%d" % n, 18490 + n), timeout=20).makefile("rwb")
    req.setdefault("jsonrpc", "2.0"); req.setdefault("id", 1)
    f.write((json.dumps(req) + "\n").encode()); f.flush(); r = json.loads(f.readline()); f.close(); return r.get("result", r)
def cl(n): return call(n, {"method": "stats"}).get("cluster") or {}
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
what = sys.argv[1]; N = int(sys.argv[2])
if what == "fill":
    f = socket.create_connection(("127.0.73.1", 18491), timeout=20).makefile("rwb")
    v = "x" * 4096
    for i in range(N):
        f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%05d" % i, "value": v}}) + "\n").encode())
        if i % 100 == 99:
            f.flush()
            for _ in range(100): f.readline()
    f.flush()
    out("ENTRIES", (call(1, {"method": "collections"}).get("collections") or [{}])[0].get("entries"))
elif what == "sweep":
    t0 = time.time(); c = {}
    while time.time() - t0 < 45:
        c = cl(1)
        if c.get("repl_sweep_cycles", 0) >= 1 and c.get("repl_sweep_sent", 0) >= N: break
        time.sleep(0.5)
    out("CYCLES", c.get("repl_sweep_cycles", "absent")); out("MS", c.get("repl_sweep_ms", "absent"))
    out("SCANNED", c.get("repl_sweep_scanned", "absent")); out("SENT", c.get("repl_sweep_sent", "absent"))
    out("B_ENTRIES", (call(2, {"method": "collections"}).get("collections") or [{}])[0].get("entries"))
elif what == "uptime":
    out("UP", (call(N, {"method": "stats"}).get("process") or {}).get("uptime_s", 0))
elif what == "reconcile":
    t0 = time.time(); peak = 0; c = {}
    while time.time() - t0 < 60:
        c = cl(1)
        peak = max(peak, c.get("reconcile_pending", 0))
        if c.get("reconcile_probed", 0) >= N and c.get("reconcile_ms", 0) > 0 and c.get("reconcile_pending", 1) == 0: break
        time.sleep(0.25)
    out("PROBED", c.get("reconcile_probed", "absent")); out("RMS", c.get("reconcile_ms", "absent"))
    out("PENDING", c.get("reconcile_pending", "absent")); out("PEAK_PENDING", peak); out("DROPPED", c.get("reconciled"))
    out("A_ENTRIES", (call(1, {"method": "collections"}).get("collections") or [{}])[0].get("entries"))
EOF
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

# both with a WAL: a WAL-less node's interchange digest differs from a
# WAL-bearing one's (S73b) and the two refuse each other
mkdir -p "$D/wal1" "$D/wal2"
conf 1 "$D/wal1"; conf 2 "$D/wal2"
start 1 || { bad "A did not start"; echo "sweepclocktest: $pass passed, $fail failed"; exit 1; }
sleep 2
python3 "$DRV" fill $N > "$D/fill.out" 2>&1
[ "$(val fill ENTRIES)" = $N ] && ok "A alone holds $N x 4 KB (4.4 MB: more than one 4 MB sweep slice)" || bad "fill: $(val fill ENTRIES) entries"

echo "--- the repair sweep's clock: B starts cold, A backfills it"
start 2 || bad "B did not start"
python3 "$DRV" sweep $N > "$D/sweep.out" 2>&1
MS=$(num "$(val sweep MS)"); SC=$(num "$(val sweep SCANNED)"); SE=$(num "$(val sweep SENT)")
[ "$(num "$(val sweep CYCLES)")" -ge 1 ] && [ "$SE" -ge $N ] && [ "$SC" -ge $N ] \
	&& ok "a completed repair cycle: $SC scanned, $SE sent, $(val sweep CYCLES) cycle(s); B holds $(val sweep B_ENTRIES)" \
	|| bad "sweep clock: cycles $(val sweep CYCLES), scanned $(val sweep SCANNED), sent $(val sweep SENT) (B $(val sweep B_ENTRIES))"
[ "$MS" -ge 9000 ] && [ "$MS" -le 25000 ] \
	&& ok "and it took $MS ms - two slices a tick apart, which is what a 4.4 MB walk on a 4 MB budget costs" \
	|| bad "repl_sweep_ms = $(val sweep MS) (want ~10000: two 10 s-spaced slices)"

echo "--- the reconcile's clock: A restarts with its WAL and probes every key it replayed"
# S223: the reconcile now runs only with a WITNESS - a peer that was up at
# least 30 s (three sweep periods) before this node went down - because
# only that peer saw what was deleted meanwhile.  B joined just before the
# sweep phase, so wait until it is old enough - 46 s, because
# this node's death is estimated from its alive stamp, touched every ten
# beats, so it can read up to 10 s EARLY (the safe direction) and the
# effective grace seen from here is up to 40 s; otherwise A would rightly
# skip the pass (a fleet that has only just formed has no witness) and
# this suite would be measuring a pass that is correctly not run.
i=0
while [ $i -lt 60 ]; do
	up=$(python3 "$DRV" uptime 2 2>/dev/null | sed -n 's/^UP=//p')
	[ "${up:-0}" -ge 46 ] 2>/dev/null && break
	sleep 1; i=$((i + 1))
done
for p in $PIDS; do grep -qa -- "n1.conf" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done
sleep 0.5
mv "$D/n1.log" "$D/n1.log.1"
start 1 || bad "A did not restart"
python3 "$DRV" reconcile $N > "$D/rec.out" 2>&1
RMS=$(num "$(val rec RMS)"); PK=$(num "$(val rec PEAK_PENDING)")
[ "$(num "$(val rec PROBED)")" -ge $N ] && [ "$(val rec PENDING)" = 0 ] && [ "$(val rec A_ENTRIES)" = $N ] \
	&& ok "the reconcile pass probed all $N replayed keys ($(val rec PROBED) issues); pending is 0 once every answer is in (peak seen $PK), $(val rec DROPPED) dropped, A still holds $N" \
	|| bad "reconcile: probed $(val rec PROBED), pending $(val rec PENDING) (peak $(val rec PEAK_PENDING)), dropped $(val rec DROPPED), A holds $(val rec A_ENTRIES)"
[ "$RMS" -ge 12000 ] && [ "$RMS" -le 30000 ] \
	&& ok "and it took $RMS ms - $N keys at 64 a second, the O(N) the filing named" \
	|| bad "reconcile_ms = $(val rec RMS) (want ~17000 for $N keys at 64/s)"
grep -q "reconcile pass finished asking - [0-9]* key" "$D/n1.log" && ok "A's log: $(grep -m1 "reconcile pass finished" "$D/n1.log" | sed 's/.*cluster: //' | cut -c1-70)" || bad "A's log has no reconcile line"

echo "sweepclocktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
