#!/bin/sh
# walobstest.sh - the daemon measures its own fsyncs and flags storage slower than its startup probe
# (which a write-back cache can fool).  Usage: test/walobstest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
# PCOBS_TMP points the fixture at a chosen filesystem (/dev/shm reproduces a ~2 us fsync)
D=$(mktemp -d "${PCOBS_TMP:-/var/tmp}/pcobs.XXXXXX")
CG=/sys/fs/cgroup/pcobs
PID=
cleanup() {
	[ -n "$PID" ] && kill -9 $PID 2>/dev/null
	if [ -d "$CG" ]; then
		while read -r p; do [ -n "$p" ] && \
			echo "$p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
		done < "$CG/cgroup.procs" 2>/dev/null
		rmdir "$CG" 2>/dev/null
	fi
	rm -rf "$D"
}
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkdir -p "$D/wal"
cat > "$D/c.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = obs-client-secret
cluster = obs-cluster-secret
[listen]
tcp = 127.0.0.1:16498
plaintext = loopback
[wal]
dir = $D/wal
probe = auto
fsync = always
segment_mb = 4
segments = 4
save = off
[collection th]
buckets_log2 = 12
CONF

# one write per round trip: the pump group-commits one fdatasync per drained batch, so a pipelined
# burst never reaches the detector's 50-sample gate
drive() {  # drive <n>
	python3 - "$1" <<'PY'
import json, pcnative, socket, sys
n = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16498), timeout=120))
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":"k%05d"%i,"value":"v"*200}})+"\n").encode())
    f.flush()
    f.readline()
PY
}
# `sync` forces a barrier and waits for it, so N calls are N fsyncs
drive_synced() {  # drive_synced <n>
	python3 - "$1" <<'PY'
import json, pcnative, socket, sys
n = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16498), timeout=180))
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":"s%05d"%i,"value":"v"*200}})+"\n").encode())
    f.flush(); f.readline()
    f.write(b'{"jsonrpc":"2.0","id":9,"method":"sync"}\n')
    f.flush(); f.readline()
PY
}

obs4() {  # one snapshot: probe_p50_us recent_us fsync_n probe_underestimated
	python3 - <<'PY'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16498), timeout=30))
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
d = json.loads(f.readline()); d = d.get("result", d)
o = d.get("wal", {}).get("observed", {})
def g(k):
    v = o.get(k); return "MISSING" if v is None else (str(v).lower() if isinstance(v, bool) else v)
print(g("probe_p50_us"), g("fsync_recent_us"), g("fsync_n"), g("probe_underestimated"))
PY
}
obs() {  # obs <field>
	python3 - "$1" <<'PY'
import json, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 16498), timeout=30))
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
d = json.loads(f.readline())
d = d.get("result", d)
o = d.get("wal", {}).get("observed", {})
v = o.get(sys.argv[1], "MISSING")
print(str(v).lower() if isinstance(v, bool) else v)
PY
}

echo "=== walobstest ==="
# real fsyncs: the detector below throttles the disk and must see them slow (no PC_NOSYNC preload)
: > "$D/log"
LD_PRELOAD= "$BIN" -f "$D/c.conf" > "$D/log" 2>&1 &
PID=$!
i=0; while [ $i -lt 600 ]; do grep -q "perfcached ready" "$D/log" && break
	sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/log" || { echo "daemon did not start"; tail -3 "$D/log"; exit 1; }

drive_synced 30
N=$(obs fsync_n); A=$(obs fsync_avg_us); M=$(obs fsync_max_us); R=$(obs fsync_recent_us)
# 30 barriers, so 30 fsyncs whatever the storage costs; ordinary writes give a count that depends
# on device speed
[ "$N" != MISSING ] && [ "$N" -ge 20 ] 2>/dev/null \
	&& ok "the daemon counted its own fsyncs (n=$N)" \
	|| bad "fsync_n absent or too low: $N"
[ "$A" != MISSING ] && [ "$A" -ge 0 ] 2>/dev/null \
	&& ok "mean fsync cost is measured (${A}us)" || bad "avg absent: $A"
[ "$M" != MISSING ] && [ "$M" -ge "$A" ] 2>/dev/null \
	&& ok "max >= mean (${M}us >= ${A}us)" || bad "max $M < avg $A"
# the flag must agree with the figures it is computed from (probe p50, recent mean, sample count,
# one snapshot): whether this host's storage is healthy is not the daemon's property
set -- $(obs4)
PP=$1; RC=$2; NN=$3; FL=$4
expect=false
[ "$PP" != MISSING ] && [ "$RC" != MISSING ] && [ "$NN" -ge 20 ] 2>/dev/null && [ "$RC" -ge $(( PP * 4 )) ] 2>/dev/null && expect=true
[ "$FL" = "$expect" ] \
	&& ok "the underestimation flag agrees with its figures: probe ${PP}us, recent ${RC}us, n $NN, flagged $FL" \
	|| bad "flag $FL disagrees with the figures (probe ${PP}us, recent ${RC}us, n $NN)"
[ "$FL" = true ] && echo "  ..   this host was contended during the phase: recent ${RC}us >= 4 x probe ${PP}us - the detector was right; the healthy-storage premise did not hold here"

# the detector: throttle after ready, the cache-fills-later shape.  Needs root, cgroup v2 and a
# whole-disk device (else SKIP, loudly); the WAL's disk is found by its parent device.
SRC=$(findmnt -n -o SOURCE --target "$D" 2>/dev/null | head -1)
PK=$(lsblk -no PKNAME "$SRC" 2>/dev/null | head -1)
DEV=$(lsblk -dno MAJ:MIN "/dev/${PK:-${SRC#/dev/}}" 2>/dev/null | tr -d ' ' | head -1)
if [ -w /sys/fs/cgroup/cgroup.procs ] && [ -n "$DEV" ] && \
   mkdir -p "$CG" 2>/dev/null && \
   echo "$DEV wiops=8 riops=500" > "$CG/io.max" 2>/dev/null; then
	echo "$PID" > "$CG/cgroup.procs" 2>/dev/null
	drive_synced 30
	sleep 2
	if grep -q "probe measured p50" "$D/log"; then
		ok "the daemon reported that the probe underestimated it"
	else
		bad "no divergence warning: recent=$(obs fsync_recent_us)us n=$(obs fsync_n) probe_p50=$(obs probe_p50_us)us"
	fi
	[ "$(obs probe_underestimated)" = true ] \
		&& ok "stats flags probe_underestimated" \
		|| bad "stats still says the probe was right"
else
	echo "  SKIP the divergence detector needs root + cgroup v2 io.max"
	echo "       (fixture not buildable here - NOT a pass)"
fi

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "walobstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
