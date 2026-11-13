#!/bin/sh
# healtest.sh - a node that loses WAL records (forced ring drops) heals itself: HEALING, an unthrottled
# snapshot, READY again, and every acknowledged write survives a kill -9 and restart.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "healtest: $SHIM is missing (make syncfailshim.so) - nothing can slow the device"; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
# a dynamic sanitizer runtime (gcc's check-asan) must come first in the
# preload list; the shim goes last (as in syncfailtest)
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
# the WAL on tmpfs where there is one: a slow disk overflows the ring even on the paced fill, which
# must land clean so the heal is the burst's
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcheal.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18491 HPORT=18492
if ss -ltn 2>/dev/null | grep -qE ":1849[12][[:space:]]"; then
	echo "healtest: port $PORT/$HPORT already bound" >&2; exit 1
fi
mkdir -p "$D/wal"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 768
[secrets]
client = hl-client
[listen]
tcp = 127.0.0.1:$PORT
http = 127.0.0.1:$HPORT
plaintext = loopback
[collection c]
buckets_log2 = 16
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 16
segments = 16
ring_kb = 16
save = off
# the default waits rather than lose a record; this test is about the heal
on_full = heal
CONF
chmod 600 "$D/n.conf"
start() {
	: > "$D/n.log"
	: > "$D/ctl"
	PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim.log" LD_PRELOAD="$PRELOAD" \
		"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n.log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"; return 1
}
start || { bad "the node did not start"; echo "healtest: $pass passed, $fail failed"; exit 1; }

python3 - "$PORT" "$HPORT" "$D" <<'PY' > "$D/run.out" 2>&1
import json, pcnative, socket, sys, threading, time, urllib.request
port, hport, D = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
def conn():
    s = socket.create_connection(("127.0.0.1", port), timeout=30); return s, pcnative.wrap(s)
def call(s, f, m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
val = lambda i: ("v%d:" % i) + "x" * 990
acked = {}
# 1. the fill, bounded by the pump (a write is acked at the ring): a sync barrier every
# 8 records (~8 KB, half the 16 KB ring) drains it however slow the pump
s, f = conn()
for b in range(0, 40000, 2):
    reqs = "".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "k%06d" % i, "value": val(i)}}) + "\n" for i in range(b, b + 2))
    f.write(reqs.encode()); f.flush()
    for i in range(b, b + 2):
        r = json.loads(f.readline())
        if (r.get("result") or {}).get("stored"): acked["k%06d" % i] = val(i)
    if b % 8 == 6: call(s, f, "sync")
st = call(s, f, "stats")["result"]
out("FILL_ACKED", len(acked)); out("FILL_DROPPED", st["wal"]["dropped"]); out("FILL_STATE", st["state"])
# the watcher: the state every 20 ms, and a canary write and read each time
seen = set(); refusals = []; reads_ok = [0]; stop = [False]
def watch():
    ws, wf = conn()
    while not stop[0]:
        st = call(ws, wf, "stats")["result"]; seen.add(st["state"])
        if st["state"] == "healing":
            r = call(ws, wf, "set", col="c", key="canary", value="x")
            if "error" in r: refusals.append(r["error"].get("message", ""))
            g = call(ws, wf, "get", col="c", key="k000001")
            if (g.get("result") or {}).get("value") == val(1): reads_ok[0] += 1
        time.sleep(0.02)
wt = threading.Thread(target=watch); wt.start()
# 2. the burst: 4 connections (both workers' rings), 5,000 x 4 KB each, pipelined with
# a writer and a reader per socket so the replies never back up into a deadlock
big = lambda i: ("b%d:" % i) + "y" * 3990
def burst(lo, n, res):
    bs, bf = conn(); rep = []
    def reader():
        for _ in range(n): rep.append(json.loads(bf.readline()))
    rt = threading.Thread(target=reader); rt.start()
    for b in range(lo, lo + n, 250):
        bf.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
            "params": {"col": "c", "key": "k%06d" % i, "value": big(i)}}) + "\n" for i in range(b, b + 250)).encode()); bf.flush()
    rt.join(); res[lo] = rep
res = {}
open(D + "/ctl", "w").write("walslow\n")         # the device slows down
bts = [threading.Thread(target=burst, args=(40000 + 5000 * k, 5000, res)) for k in range(4)]
[t.start() for t in bts]; [t.join() for t in bts]
open(D + "/ctl", "w").write("")                  # and recovers
burst_ok = 0
for lo, rep in res.items():
    for i, r in zip(range(lo, lo + 5000), rep):
        if (r.get("result") or {}).get("stored"): acked["k%06d" % i] = big(i); burst_ok += 1
out("BURST_ACKED", burst_ok)
# 3. wait for the heal to finish - ALL of it: READY can show between two heals;
# wal.unhealed counts the losses no heal has covered yet, 0 means done
t0 = time.time(); st = {}; early = 0
while time.time() - t0 < 60:
    st = call(s, f, "stats")["result"]
    if st["state"] != "healing" and st["wal"].get("heals", 0) >= 1:
        if st["wal"]["unhealed"] == 0: break
        early += 1
    time.sleep(0.1)
out("READY_UNHEALED", early)
time.sleep(0.3); stop[0] = True; wt.join()
out("DROPPED", st["wal"]["dropped"]); out("OVERRUNS", st["wal"]["overruns"])
out("STATE_AFTER", st["state"]); out("HEALS", st["wal"].get("heals", 0)); out("LAST_HEAL_MS", st["wal"].get("last_heal_ms"))
out("UNHEALED", st["wal"].get("unhealed"))
out("SEEN", ",".join(sorted(seen))); out("REFUSALS", len(refusals))
out("REFUSAL_MSG", (refusals[0] if refusals else "")[:60]); out("READS_WHILE_HEALING", reads_ok[0])
# 4. writes again, and /metrics
r = call(s, f, "set", col="c", key="after", value="yes"); ok_after = bool((r.get("result") or {}).get("stored"))
if ok_after: acked["after"] = "yes"
out("WRITE_AFTER", ok_after)
# before the kill -9 below: an ack is at the producer ring, and kill -9 takes what
# the WAL thread has not drained
call(s, f, "sync")
m = urllib.request.urlopen("http://127.0.0.1:%d/metrics" % hport, timeout=5).read().decode()
out("M_READY", 'perfcached_node_state{state="ready"} 1' in m)
out("M_HEALS", [l for l in m.splitlines() if l.startswith("perfcached_wal_heals_total ")][:1])
json.dump(acked, open(D + "/acked.json", "w"))
out("ACKED_TOTAL", len(acked))
PY
val() { sed -n "s/^$1=//p" "$D/run.out" | head -1; }
num() { case "$1" in ''|*[!0-9]*) echo 0;; *) echo "$1";; esac; }

[ "$(val FILL_ACKED)" = 40000 ] && [ "$(val FILL_DROPPED)" = 0 ] && [ "$(val FILL_STATE)" = ready ] \
	&& ok "1. a fill of 40,000 x 1 KB, synced every 8, lands, nothing dropped" \
	|| bad "1. fill: acked $(val FILL_ACKED), dropped $(val FILL_DROPPED), state $(val FILL_STATE) $(tail -3 "$D/run.out" | tr '\n' ' ')"
DR=$(num "$(val DROPPED)")
[ "$DR" -gt 0 ] \
	&& ok "2. the burst overflowed the 16 KB rings: $DR record(s) dropped, $(val OVERRUNS) overrun(s) (the loss is real)" \
	|| bad "2. nothing was dropped - this run cannot show a heal"
case ",$(val SEEN)," in *,healing,*)
	ok "3. the node was seen HEALING (states seen: $(val SEEN))";;
	*) bad "3. never seen HEALING (states seen: $(val SEEN))";; esac
case "$(val REFUSAL_MSG)" in "node is HEALING"*)
	ok "   and while it healed a write was refused with the heal's own message, and $(val READS_WHILE_HEALING) read(s) were served";;
	*) bad "   no write refused as HEALING ($(val REFUSALS) refusal(s): '$(val REFUSAL_MSG)')";; esac
[ "$(val STATE_AFTER)" = ready ] && [ "$(num "$(val HEALS)")" -ge 1 ] && [ "$(val UNHEALED)" = 0 ] \
	&& ok "   it returned to READY by itself: $(val HEALS) heal(s), the last $(val LAST_HEAL_MS) ms, nothing left unhealed (READY with a heal still owed seen $(val READY_UNHEALED) time(s))" \
	|| bad "   after the burst: state $(val STATE_AFTER), heals $(val HEALS), unhealed $(val UNHEALED)"
grep -q "HEALING: refusing writes" "$D/n.log" && grep -q "wal: healed after" "$D/n.log" \
	&& ok "   the log says both: $(grep -m1 -o "healed after [0-9]* ms" "$D/n.log")" \
	|| bad "   the log: $(grep -m2 -E "HEALING|healed|FAILED" "$D/n.log" | cut -c1-120 | tr '\n' '|')"
[ "$(val WRITE_AFTER)" = True ] && [ "$(val M_READY)" = True ] \
	&& ok "4. it takes writes again; /metrics: node_state ready, $(val M_HEALS)" \
	|| bad "4. write after $(val WRITE_AFTER), metrics ready $(val M_READY) $(val M_HEALS)"

# ---- 5. the point: every acknowledged write survives a kill -9 ----------
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
start || bad "5. the node did not restart"
R=$(python3 - "$PORT" "$D" <<'PY'
import json, pcnative, socket, sys
port, D = int(sys.argv[1]), sys.argv[2]
acked = json.load(open(D + "/acked.json"))
s = socket.create_connection(("127.0.0.1", port), timeout=30); f = pcnative.wrap(s)
keys = list(acked); missing = wrong = 0
for b in range(0, len(keys), 200):
    chunk = keys[b:b + 200]
    f.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "get",
        "params": {"col": "c", "key": k}}) + "\n" for i, k in enumerate(chunk)).encode()); f.flush()
    for k in chunk:
        r = json.loads(f.readline()).get("result")
        if r is None or "value" not in (r or {}): missing += 1
        elif r["value"] != acked[k]: wrong += 1
print("acked %d missing %d wrong %d" % (len(acked), missing, wrong))
PY
)
case "$R" in *" missing 0 wrong 0")
	ok "5. after kill -9 and a restart every acknowledged write is there ($R) - the dropped ones included";;
	*) bad "5. after the restart: $R";; esac

echo "healtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
