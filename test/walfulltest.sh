#!/bin/sh
# walfulltest.sh - S466: a WAL that cannot take a write makes the writer
# wait; it neither loses the record nor refuses writes - and /stats answers
# meanwhile.
#
# Before S466 a full log was a loss: a segment no snapshot had covered was
# overwritten (wal: FULL) and the node healed (S229), refusing writes until
# a snapshot landed - measured on 207 with 4 x 1 MB segments: 3,894 of a
# 40,000-key fill stored, the rest refused.  Under on_full = wait (the
# default) the pump drains only what the active segment holds and waits for
# the snapshot it asks for; the rings fill, and a connection that writes is
# held unread (writer_pauses) while the worker serves its others.
#   1. on_full = wait, 4 x 1 MB segments, save = off, 40,000 x 1 KB: no
#      overwrite, no heal, the full-log wait counted, every key stored and
#      every key back after kill -9 and a restart;
#   2. the control - the same with on_full = heal - must overwrite, or arm 1
#      proved nothing;
#   3. the device slowed (syncfailshim walslow), 4,000 x 4 KB: nothing
#      dropped, writers held, and /stats on another connection answers in
#      under a second throughout (207: 8 ms at most; 2.0 s when a waiting
#      write held the worker, before the pause).
# usage: test/walfulltest.sh [./perfcached] [./pcbench] [./perfcli] [./syncfailshim.so]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
PB=${2:-./pcbench}
CLI=${3:-./perfcli}
SHIM=${4:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "walfulltest: $SHIM is missing (make syncfailshim.so)"; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
PORT=${PORT:-17871}
D=$(mktemp -d /var/tmp/pcwalfull.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
[ -z "$REV" ] || [ "$REV" = unknown ] && { echo "walfulltest: build cannot name itself"; exit 2; }
ss -ltn 2>/dev/null | grep -q ":$PORT[[:space:]]" && { echo "walfulltest: port $PORT already bound" >&2; exit 1; }

mkcfg() { # mkcfg <on_full> <extra>
	cat > "$D/n.conf" <<CFG
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = wf-secret
cluster = wf-cluster
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
save = off
on_full = $1
$2
[collection c]
buckets_log2 = 17
CFG
	chmod 640 "$D/n.conf"
}
start() { # start [env ...] - the daemon on $D/n.conf, logging to $D/n.log
	env "$@" "$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	P=$!
	k=0
	while [ $k -lt 300 ]; do
		[ "$(grep -c "perfcached ready" "$D/n.log")" -ge "$NREADY" ] && return 0
		sleep 0.1; k=$((k+1))
	done
	echo "  the node did not start:"; tail -3 "$D/n.log"; return 1
}
wal() { "$CLI" -h 127.0.0.1 -p "$PORT" -j '{"method":"stats"}' 2>/dev/null | python3 -c "import json,sys
try:
    d=json.load(sys.stdin); w=d['wal']
    print(w['appended'], w['dropped'], w['overruns'], w['heals'], w.get('full_waits', -1),
          w.get('writer_pauses', -1), d['collections'][0]['entries'])
except Exception: print('-1 -1 -1 -1 -1 -1 -1')"; }

echo "=== walfulltest ($REV) ==="

# 1. on_full = wait: the full log waits, nothing is lost or refused
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/n.log"; NREADY=1
mkcfg wait "segments = 4
segment_mb = 1"
start || exit 1
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n 40000 -v 1024 -c 4 -d 32 >/dev/null 2>&1
sleep 2
set -- $(wal)
echo "  wait:     appended=$1 dropped=$2 overruns=$3 heals=$4 full_waits=$5 entries=$7"
[ "$3" = 0 ] && [ "$4" = 0 ] && ! grep -q "wal: FULL" "$D/n.log" \
	&& ok "1. no segment overwritten, no heal" \
	|| bad "1. overruns $3, heals $4, FULL lines $(grep -c 'wal: FULL' "$D/n.log") (want none)"
[ "$5" -gt 0 ] 2>/dev/null && grep -q "writes wait for the snapshot" "$D/n.log" \
	&& ok "1. the full log was waited out ($5 time(s)), and said so" \
	|| bad "1. full_waits $5 - the log never filled, so the arm proves nothing"
[ "$7" = 40000 ] && [ "$1" = 40000 ] && [ "$2" = 0 ] && ok "1. every write stored and logged (40000)" \
	|| bad "1. entries $7, appended $1, dropped $2 of 40000"
kill -9 $P 2>/dev/null; wait $P 2>/dev/null; P=
NREADY=2
start || exit 1
set -- $(wal)
[ "$7" = 40000 ] && ok "1. after kill -9 and a restart: 40000 keys" \
	|| bad "1. after kill -9 and a restart: $7 keys of 40000"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

# 2. the control: on_full = heal overwrites or heals on the same load
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/n.log"; NREADY=1
mkcfg heal "segments = 4
segment_mb = 1"
start || exit 1
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n 40000 -v 1024 -c 4 -d 32 >/dev/null 2>&1
sleep 2
set -- $(wal)
echo "  heal:     appended=$1 overruns=$3 heals=$4 entries=$7"
# overwrote, or healed first (GitHub durability, 41297001399: heals 1, overruns 0)
{ [ "$3" -gt 0 ] 2>/dev/null || [ "$4" -gt 0 ] 2>/dev/null; } && grep -qE "wal: (FULL|DROPPED|dropped|healed)" "$D/n.log" \
	&& ok "2. the control filled the log ($3 overrun(s), $4 heal(s)) - the load fills this log" \
	|| bad "2. the control never filled the log (overruns $3, heals $4) - arm 1's wait proves nothing on this runner"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

# 3. /stats answers while the writers are held
rm -rf "$D/wal"; mkdir -p "$D/wal"; : > "$D/n.log"; : > "$D/ctl"; : > "$D/shim.log"; NREADY=1
mkcfg wait "ring_kb = 64"
start PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim.log" LD_PRELOAD="$PRELOAD" || exit 1
echo walslow > "$D/ctl"
"$PB" -h 127.0.0.1 -p "$PORT" -P perf -C c -F -n 4000 -v 4096 -c 4 -d 32 >/dev/null 2>&1 &
PBP=$!
LAT=$(python3 - "$CLI" "$PORT" <<'PY'
import subprocess, sys, time
t_end = time.time() + 15
lat = []
while time.time() < t_end:
    t0 = time.time()
    try:
        subprocess.run([sys.argv[1], "-h", "127.0.0.1", "-p", sys.argv[2], "-q", "-j",
                        '{"method":"stats"}'], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
    except subprocess.TimeoutExpired:
        pass
    lat.append(time.time() - t0)
    time.sleep(0.25)
print("%d %.3f" % (len(lat), max(lat)))
PY
)
k=0
while kill -0 $PBP 2>/dev/null && [ $k -lt 240 ]; do sleep 0.5; k=$((k+1)); done
kill $PBP 2>/dev/null; wait $PBP 2>/dev/null
: > "$D/ctl"
sleep 2
set -- $(wal)
echo "  slowed:   appended=$1 dropped=$2 heals=$4 writer_pauses=$6 entries=$7; /stats: $LAT (calls, slowest s)"
grep -q walslow "$D/shim.log" && ok "3. the shim slowed the WAL" || bad "3. the shim never slowed a WAL write - the arm proves nothing"
[ "$2" = 0 ] && [ "$4" = 0 ] && [ "$7" = 4000 ] && ok "3. nothing dropped, no heal, 4000 stored" \
	|| bad "3. dropped $2, heals $4, entries $7 of 4000"
[ "$6" -gt 0 ] 2>/dev/null && ok "3. writers were held ($6 pause(s))" \
	|| bad "3. writer_pauses $6 - no connection was held, so the arm proves nothing"
python3 -c "import sys; n, m = sys.argv[1].split(); sys.exit(0 if int(n) >= 10 and float(m) < 1.0 else 1)" "$LAT" \
	&& ok "3. /stats answered in under a second throughout ($LAT)" \
	|| bad "3. /stats while writers were held: $LAT (calls, slowest s) - want 10+ calls, all under 1 s"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

echo "walfulltest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
