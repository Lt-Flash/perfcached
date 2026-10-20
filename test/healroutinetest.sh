#!/bin/sh
# healroutinetest.sh - S306 step 3: a node that heals again and again says
# healing has become routine (the log is undersized for the write rate).
#
# S229 heals a WAL loss in place - writes refused, a snapshot, READY - and
# the operator kept it (DESIGN 12fx, 12ia): clients never wait.  One heal
# is an incident; PC_WAL_HEAL_ROUTINE (3) within PC_WAL_HEAL_WIN_S (600 s)
# is the load, every time it returns.  One node, 16 KB rings, the device
# slowed (syncfailshim "walslow") during each of four bursts, so every
# burst loses records and heals:
#   1. after the FIRST heal nothing says routine (heals_recent 1, false)
#   2. after four heals: /stats wal.heals_recent >= 3, heal_routine true
#   3. /metrics perfcached_wal_heal_routine 1, heals_recent >= 3
#   4. the WARNING "healing has become routine" appears exactly ONCE
#      (once per window, not once per heal)
#   5. the node is READY afterwards with nothing unhealed
# Fail-first: before step 3 the fields and the line do not exist.
# Usage: test/healroutinetest.sh [./perfcached] [./syncfailshim.so]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "healroutinetest: $SHIM is missing (make syncfailshim.so)"; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pchr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18495 HPORT=18496
if ss -ltn 2>/dev/null | grep -qE ":1849[56][[:space:]]"; then
	echo "healroutinetest: port $PORT/$HPORT already bound" >&2; exit 1
fi
mkdir -p "$D/wal" "$D/state"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 256
[secrets]
client = hr-client
[listen]
tcp = 127.0.0.1:$PORT
http = 127.0.0.1:$HPORT
plaintext = loopback
[collection c]
buckets_log2 = 14
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 16
segments = 16
ring_kb = 16
save = off
CONF
chmod 600 "$D/n.conf"
: > "$D/n.log"; : > "$D/ctl"
PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim.log" PC_SYNCFAIL_SLOW_MS=50 \
	LD_PRELOAD="$PRELOAD" "$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 300 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	kill -0 $PID 2>/dev/null || break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { bad "the node did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; echo "healroutinetest: $pass passed, $fail failed"; exit 1; }

python3 - "$PORT" "$HPORT" "$D" <<'PY' > "$D/run.out" 2>&1
import json, pcnative, socket, sys, time, urllib.request
port, hport, D = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
def stats():
    return json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % hport, timeout=10).read())
def metric(name):
    for l in urllib.request.urlopen("http://127.0.0.1:%d/metrics" % hport, timeout=10).read().decode().splitlines():
        if l.startswith(name + " "): return l.split()[1]
    return None
def heal_once(n):
    """one burst against a slowed device, then wait until it has healed"""
    open(D + "/ctl", "w").write("walslow\n")
    s = socket.create_connection(("127.0.0.1", port), timeout=60); f = pcnative.wrap(s)
    v = "y" * 4000
    reqs = "".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "b%d-%d" % (n, i), "value": v}}) + "\n" for i in range(400))
    f.write(reqs.encode()); f.flush()
    for _ in range(400): f.readline()
    s.close()
    open(D + "/ctl", "w").write("")
    h0 = n - 1
    t = time.time()
    while time.time() - t < 60:
        st = stats(); w = st["wal"]
        if w.get("heals", 0) > h0 and st["state"] == "ready" and not w.get("healing"):
            return w
        time.sleep(0.1)
    return stats()["wal"]
w = heal_once(1)
out("H1_HEALS", w.get("heals")); out("H1_RECENT", w.get("heals_recent")); out("H1_ROUTINE", w.get("heal_routine"))
for n in (2, 3, 4):
    w = heal_once(n)
st = stats(); w = st["wal"]
out("HEALS", w.get("heals")); out("RECENT", w.get("heals_recent")); out("ROUTINE", w.get("heal_routine"))
out("STATE", st["state"]); out("UNHEALED", w.get("unhealed"))
out("M_ROUTINE", metric("perfcached_wal_heal_routine")); out("M_RECENT", metric("perfcached_wal_heals_recent"))
PY
v() { sed -n "s/^$1=//p" "$D/run.out" | tail -1; }
grep -q '^HEALS=' "$D/run.out" || { bad "the driver did not finish: $(tail -3 "$D/run.out" | tr '\n' ' ')"; echo "healroutinetest: $pass passed, $fail failed"; exit 1; }

S=$(grep -c . "$D/shim.log" 2>/dev/null || echo 0)
[ "$(v H1_HEALS)" = 1 ] && [ "$(v H1_RECENT)" = 1 ] && [ "$(v H1_ROUTINE)" = False ] \
	&& ok "1. one heal is an incident: heals_recent 1, heal_routine false (the slowed device fired $S time(s))" \
	|| bad "1. after the first heal: heals $(v H1_HEALS), heals_recent $(v H1_RECENT), heal_routine $(v H1_ROUTINE) (shim fired $S time(s))"
[ "$(v HEALS)" -ge 4 ] 2>/dev/null && [ "$(v RECENT)" -ge 3 ] 2>/dev/null && [ "$(v ROUTINE)" = True ] \
	&& ok "2. after $(v HEALS) heals: /stats heals_recent $(v RECENT), heal_routine true" \
	|| bad "2. after the bursts: heals $(v HEALS), heals_recent $(v RECENT), heal_routine $(v ROUTINE)"
[ "$(v M_ROUTINE)" = 1 ] && [ "$(v M_RECENT)" -ge 3 ] 2>/dev/null \
	&& ok "3. /metrics perfcached_wal_heal_routine 1, perfcached_wal_heals_recent $(v M_RECENT)" \
	|| bad "3. /metrics heal_routine $(v M_ROUTINE), heals_recent $(v M_RECENT)"
n=$(grep -c "healing has become routine" "$D/n.log")
[ "$n" = 1 ] && ok "4. the WARNING said it once: $(grep -o 'wal: [0-9]* heals in the last [0-9]* minutes' "$D/n.log" | head -1)" \
	|| bad "4. 'healing has become routine' logged $n time(s), want exactly 1"
[ "$(v STATE)" = ready ] && [ "$(v UNHEALED)" = 0 ] \
	&& ok "5. READY afterwards, nothing unhealed" \
	|| bad "5. state $(v STATE), unhealed $(v UNHEALED)"
echo "healroutinetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
