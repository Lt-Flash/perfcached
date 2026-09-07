#!/bin/sh
# walobstest.sh — S63 part 3: the daemon measures its OWN fsyncs.
#
# The startup probe writes at most 16 MB in ~1.2s, so on storage with a
# write-back cache in front of it - every VM running cache=writeback -
# it measures the cache and reports a latency the device never delivers
# under load.  Measured 2026-09-03 on Ceph: probe 139us p50, sustained
# fio 3.7ms.  Only the running daemon is still measuring when that cache
# fills, so it has to compare and say so.
# Usage: test/walobstest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
# PCOBS_TMP points the fixture at a chosen filesystem.  The count
# assertion below broke on a CI runner whose fsync is ~2us, and without
# this there was no way to reproduce that speed locally - /dev/shm is
# the same profile.
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

# One write per round trip.  The pump group-commits - one fdatasync per
# DRAINED BATCH, not per record - so a pipelined burst of 200 writes
# produced only TWO fsyncs and never came near the detector's 50-sample
# gate.  Waiting for each reply forces each write into its own batch.
drive() {  # drive <n>
	python3 - "$1" <<'PY'
import json, socket, sys
n = int(sys.argv[1])
f = socket.create_connection(("127.0.0.1", 16498), timeout=120).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":"k%05d"%i,"value":"v"*200}})+"\n").encode())
    f.flush()
    f.readline()
PY
}
# `sync` forces a barrier and WAITS for it, so N calls are N fsyncs.
# Needed because the pump group-commits: under a throttle a burst of 250
# writes drained as two batches and produced two fsyncs, so no amount of
# extra traffic could reach the detector's sample gate.
drive_synced() {  # drive_synced <n>
	python3 - "$1" <<'PY'
import json, socket, sys
n = int(sys.argv[1])
f = socket.create_connection(("127.0.0.1", 16498), timeout=180).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"th","key":"s%05d"%i,"value":"v"*200}})+"\n").encode())
    f.flush(); f.readline()
    f.write(b'{"jsonrpc":"2.0","id":9,"method":"sync"}\n')
    f.flush(); f.readline()
PY
}

obs() {  # obs <field>
	python3 - "$1" <<'PY'
import json, socket, sys
f = socket.create_connection(("127.0.0.1", 16498), timeout=30).makefile("rwb")
f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
d = json.loads(f.readline())
d = d.get("result", d)
o = d.get("wal", {}).get("observed", {})
v = o.get(sys.argv[1], "MISSING")
print(str(v).lower() if isinstance(v, bool) else v)
PY
}

echo "=== walobstest ==="
"$BIN" -f "$D/c.conf" > "$D/log" 2>&1 &
PID=$!
i=0; while [ $i -lt 600 ]; do grep -q "perfcached ready" "$D/log" && break
	sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/log" || { echo "daemon did not start"; tail -3 "$D/log"; exit 1; }

drive_synced 30
N=$(obs fsync_n); A=$(obs fsync_avg_us); M=$(obs fsync_max_us); R=$(obs fsync_recent_us)
# 30 barriers, so 30 fsyncs whatever the storage costs.  Counting the
# fsyncs that ordinary writes happen to produce is NOT portable: the
# pump group-commits, so the number falls out of device speed - this
# assertion passed on 1.5ms storage and failed on a 2us CI runner where
# seventy writes drained as two batches.
[ "$N" != MISSING ] && [ "$N" -ge 20 ] 2>/dev/null \
	&& ok "the daemon counted its own fsyncs (n=$N)" \
	|| bad "fsync_n absent or too low: $N"
[ "$A" != MISSING ] && [ "$A" -ge 0 ] 2>/dev/null \
	&& ok "mean fsync cost is measured (${A}us)" || bad "avg absent: $A"
[ "$M" != MISSING ] && [ "$M" -ge "$A" ] 2>/dev/null \
	&& ok "max >= mean (${M}us >= ${A}us)" || bad "max $M < avg $A"
[ "$(obs probe_underestimated)" = false ] \
	&& ok "healthy storage: probe not flagged" \
	|| bad "probe flagged as wrong on unthrottled storage"

# The detector itself.  Probe ran unthrottled (fast); throttling AFTER
# the daemon is ready is exactly the cache-fills-later shape.  Needs
# root + cgroup v2 + a whole-disk device: an unbuildable fixture is a
# SKIP said loudly, never a silent pass.
DEV=$(lsblk -no MAJ:MIN,MOUNTPOINT 2>/dev/null | awk '$2=="/"{print $1}' | head -1)
DEV=$(echo "$DEV" | sed 's/:[0-9]*$/:0/;s/^\([0-9]*\):.*/\1:0/')
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
