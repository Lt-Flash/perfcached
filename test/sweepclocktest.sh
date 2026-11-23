#!/bin/sh
# sweepclocktest.sh - the repair sweep and the post-restart reconcile carry a clock (repl_sweep_*,
# reconcile_ms/_pending) that reads what the last completed walk cost.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
# the one-by-one reconcile a restart or a floor ran before 12ol is this suite's subject; the default
# now copies a survivor's state whole (rejoinwitnesstest)
export PERFCACHED_TEST_RECONCILE=1
D=$(mktemp -d /var/tmp/pcsw.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
# this suite times the walk at 64 a second
reconcile_rate_max = 64
collections = c
# the digest gate off: with it, the fresh peer's first cycle finds B holding every
# record and walks nothing, so the clock never ticks (sweepgatetest covers the gate)
sweep_digests = no
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
	: > "$D/n$1.log"
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
import json, pcnative, socket, sys, time
def call(n, req):
    f = pcnative.wrap(socket.create_connection(("127.0.73.%d" % n, 18490 + n), timeout=20))
    req.setdefault("jsonrpc", "2.0"); req.setdefault("id", 1)
    f.write((json.dumps(req) + "\n").encode()); f.flush(); r = json.loads(f.readline()); f.close(); return r.get("result", r)
def cl(n): return call(n, {"method": "stats"}).get("cluster") or {}
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
what = sys.argv[1]; N = int(sys.argv[2])
if what == "fill":
    f = pcnative.wrap(socket.create_connection(("127.0.73.1", 18491), timeout=20))
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
    out("PROBED", c.get("reconcile_probed", "absent")); out("RMS", c.get("reconcile_ms", "absent")); out("ROUNDS", c.get("reconcile_rounds", 0))
    out("PENDING", c.get("reconcile_pending", "absent")); out("PEAK_PENDING", peak); out("DROPPED", c.get("reconciled"))
    out("A_ENTRIES", (call(1, {"method": "collections"}).get("collections") or [{}])[0].get("entries"))
EOF
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

# both with a WAL: a WAL-less node's interchange digest differs from a WAL-bearing one's
# and the two refuse each other
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
# the reconcile runs only with a witness up 30 s before this node died, and the death is read from
# an alive stamp up to 10 s early: wait 46 s, or A rightly skips the pass
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
# a probe that timed out walks the pass again, and the clock reads every walk
W=$(( $(num "$(val rec ROUNDS)") + 1 ))
[ "$RMS" -ge 12000 ] && [ "$RMS" -le $((30000 * W)) ] \
	&& ok "and it took $RMS ms over $W walk(s) - $N keys at 64 a second, the O(N) the filing named" \
	|| bad "reconcile_ms = $(val rec RMS) over $W walk(s) (want ~17000 a walk for $N keys at 64/s)"
grep -q "reconcile pass finished asking - [0-9]* key" "$D/n1.log" && ok "A's log: $(grep -m1 "reconcile pass finished" "$D/n1.log" | sed 's/.*cluster: //' | cut -c1-70)" || bad "A's log has no reconcile line"

echo "sweepclocktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
