#!/bin/sh
# rdbkilltest.sh - RV-14: a node killed while a snapshot is being written.
#
# Snapshots were tested for landing atomically when they finish, and nodes
# for surviving kill -9 at random moments - but nothing killed a node WHILE
# a snapshot was in flight.  That is where three things must hold:
#   1. recovery uses the last PUBLISHED snapshot plus the WAL after its
#      marker, so every acknowledged write is there - the ones made while
#      the killed snapshot ran included;
#   2. while that snapshot ran, the WAL ring wrapped and recycled only what
#      the published snapshot covers, never what the half-written one would
#      have (seg_hot against safe_marker, S215) - the ring here is sized so
#      it DOES wrap during the save, or this arm would test nothing;
#   3. the dead snapshot's temp file does not outlive the restart.  It did:
#      nothing removed PC_RDB_FILE.tmp.<pid>, a table-sized file per crash
#      on the WAL's volume - and because it is opened O_EXCL under the pid,
#      a daemon back with the SAME pid (always, as pid 1 in a container)
#      could never write a snapshot again, nor, since S229, heal.
# Arms: kill -9 mid-save twice (debris would accumulate), then a stale temp
# planted under the running daemon's own pid (the same-pid case): the next
# snapshot must still succeed.
# Not an arm, and why: "killed between the rename and the directory fsync"
# is a POWER-loss property; a killed process cannot lose a rename the
# kernel has already made, so kill -9 cannot test it.
# FAIL-FIRST: the build before RV-14 keeps both temp files and fails the
# snapshot under the planted name.
# Usage: test/rdbkilltest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcrk.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18497
if ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]"; then
	echo "rdbkilltest: port $PORT already bound" >&2; exit 1
fi
mkdir -p "$D/wal"
# a 24 MB ring: the fill before the first snapshot (10 MB) plus everything
# after it (~16 MB) wraps it during the second save, without ever holding
# more than it can since the published marker; snapshots throttled to
# 8 MB/s so the second one (~22 MB) is in flight for seconds
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = rk-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = 16
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 1
segments = 24
ring_kb = 16384
save = off
rdb_mb_s = 8
CONF
chmod 600 "$D/n.conf"
start() {
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 600 ]; do
		grep -q "perfcached ready" "$D/n.log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"; return 1
}
temps() { ls "$D/wal" 2>/dev/null | grep -c "^dump.rdb.tmp\." ; }
DRV="$D/drv.py"
cat > "$DRV" <<'PY'
# drv.py <port> <acked-file> <op> ... : the node's side of each step
import json, os, socket, sys, time
port, ackf, op = int(sys.argv[1]), sys.argv[2], sys.argv[3]
s = socket.create_connection(("127.0.0.1", port), timeout=30); f = s.makefile("rb")
def call(m, **p):
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode())
    return json.loads(f.readline())
def stats(): return call("stats")["result"]
val = lambda i: ("v%d:" % i) + "x" * 990
def fill(lo, hi):
    acked = []
    for b in range(lo, hi, 8):
        e = min(hi, b + 8)
        s.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
            "params": {"col": "c", "key": "k%06d" % i, "value": val(i)}}) + "\n" for i in range(b, e)).encode())
        for i in range(b, e):
            if (json.loads(f.readline()).get("result") or {}).get("stored"): acked.append(i)
    with open(ackf, "a") as a: a.write("".join("%d\n" % i for i in acked))
    return len(acked)
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
if op == "prime":                  # fill A, then a published first snapshot
    out("A", fill(0, 10000))
    call("save")
    for _ in range(300):
        if stats()["rdb"]["saves"] >= 1 and not stats()["rdb"]["running"]: break
        time.sleep(0.1)
    out("SAVES", stats()["rdb"]["saves"])
elif op == "midsave":              # fill B, start a slow save, write C during it
    lo = int(sys.argv[4])
    out("B", fill(lo, lo + 12000))
    r0 = stats()["wal"]["recycles"]
    out("STARTED", call("save").get("result", {}).get("started"))
    time.sleep(0.3)
    out("C", fill(lo + 12000, lo + 16000))
    st = stats()
    out("RUNNING", st["rdb"]["running"]); out("RECYCLED_DURING", st["wal"]["recycles"] - r0)
    out("OVERRUNS", st["wal"]["overruns"]); out("DROPPED", st["wal"]["dropped"])
elif op == "verify":
    acked = sorted({int(x) for x in open(ackf).read().split()})
    miss = wrong = 0
    for b in range(0, len(acked), 200):
        chunk = acked[b:b + 200]
        s.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "get",
            "params": {"col": "c", "key": "k%06d" % i}}) + "\n" for i in chunk).encode())
        for i in chunk:
            r = json.loads(f.readline()).get("result")
            if not r or "value" not in r: miss += 1
            elif r["value"] != val(i): wrong += 1
    print("acked %d missing %d wrong %d" % (len(acked), miss, wrong))
elif op == "save":                 # one snapshot, to completion
    n0 = stats()["rdb"]["saves"]; e0 = stats()["rdb"]["save_errors"]
    call("save")
    for _ in range(600):
        st = stats()["rdb"]
        if (st["saves"] > n0 or st["save_errors"] > e0) and not st["running"]: break
        time.sleep(0.1)
    st = stats()["rdb"]
    print("saves +%d errors +%d" % (st["saves"] - n0, st["save_errors"] - e0))
PY
start || { bad "the node did not start"; echo "rdbkilltest: $pass passed, $fail failed"; exit 1; }
P=$(python3 "$DRV" $PORT "$D/acked" prime)
case "$P" in *"A=10000"*"SAVES=1"*) ok "10,000 x 1 KB written and a first snapshot published";;
	*) bad "prime: $P";; esac

for round in 1 2; do
	lo=$((10000 + (round - 1) * 16000))
	M=$(python3 "$DRV" $PORT "$D/acked" midsave $lo)
	v() { echo "$M" | sed -n "s/^$1=//p"; }
	[ "$(v RUNNING)" = True ] && [ "$(v DROPPED)" = 0 ] && [ "$(v OVERRUNS)" = 0 ] \
		&& ok "round $round: a snapshot in flight after 12,000 more writes and 4,000 during it (0 dropped, 0 overruns)" \
		|| bad "round $round: the save was not in flight when killed, or the load did not land: $(echo "$M" | tr '\n' ' ')"
	[ "$(v RECYCLED_DURING)" -gt 0 ] 2>/dev/null \
		&& ok "   and the WAL wrapped while it ran ($(v RECYCLED_DURING) segment(s) recycled) - arm 2 is live" \
		|| bad "   the WAL did not wrap during the save ($(v RECYCLED_DURING)) - nothing tested the recycling"
	[ "$(temps)" -ge 1 ] && ok "   its temp file is on disk: $(ls "$D/wal" | grep "^dump.rdb.tmp\." | tr '\n' ' ')" \
		|| bad "   no temp file on disk while the save ran"
	kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
	start || { bad "   the node did not restart"; break; }
	[ "$(temps)" = 0 ] \
		&& ok "   killed mid-save and restarted: no temp file left ($(grep -c 'removed [0-9]* temp file' "$D/n.log") sweep line(s) in the log)" \
		|| bad "   killed mid-save and restarted: $(temps) dead temp file(s) left on the WAL's volume"
	R=$(python3 "$DRV" $PORT "$D/acked" verify)
	case "$R" in *" missing 0 wrong 0")
		ok "   every acknowledged write is there ($R) - the ones written during the killed save included";;
		*) bad "   after the kill: $R";; esac
done

# the same-pid case: a dead temp under THIS process's name
: > "$D/wal/dump.rdb.tmp.$PID"
S=$(python3 "$DRV" $PORT "$D/acked" save)
[ "$S" = "saves +1 errors +0" ] \
	&& ok "a dead temp under the daemon's own pid (a container's pid 1 after a crash): the next snapshot still lands ($S)" \
	|| bad "a dead temp under the daemon's own pid blocks snapshots: $S - $(grep -m1 'cannot create' "$D/n.log" | cut -c1-120)"

echo "rdbkilltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
