#!/bin/sh
# walprobetest.sh — S12 verification: the probe measures with the WAL's
# own I/O pattern and the policy pairs measurement with identity honestly
# (a blazing tmpfs number must never read as durable).
#
# S63 (2026-09-03) removed the per-device cache: it was keyed by a device
# id that a hypervisor-side storage migration does not change, so it
# served numbers for storage that no longer existed.  These assertions
# were inverted with it - every start must MEASURE, and the reported
# ceiling must say it is an upper bound rather than a sustained rate.
# Usage: test/walprobetest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwp.XXXXXX)      # disk-backed (not tmpfs /tmp)
SHM=/dev/shm/pcwp.$$
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D" "$SHM"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

# tmpfs: numbers are blazing AND the policy refuses to call them durable
mkdir -p "$SHM"
R=$("$BIN" -P "$SHM")
echo "--- tmpfs probe ---"; echo "$R"
echo "$R" | grep -q "failure class memory" && ok || bad "tmpfs identity"
echo "$R" | grep -q "wal: probe" && ok || bad "tmpfs probe line"
echo "$R" | grep -q "NOT durability" && ok || bad "tmpfs durability honesty"

# disk-backed dir: fresh probe with sane numbers
R=$("$BIN" -P "$D")
echo "--- disk probe (fresh) ---"; echo "$R"
echo "$R" | grep -q "wal: probe (cached)" && bad "first run claimed cached" || ok
P50=$(echo "$R" | sed -n 's/.*p50 \([0-9]*\)us.*/\1/p')
P99=$(echo "$R" | sed -n 's/.*p99 \([0-9]*\)us.*/\1/p')
IOPS=$(echo "$R" | sed -n 's/.* \([0-9]*\) iops (4KB+fdatasync, QD1.*/\1/p')
[ -n "$P50" ] && [ -n "$P99" ] && [ "$P99" -ge "$P50" ] && ok \
	|| bad "percentiles insane: p50=$P50 p99=$P99"
[ -n "$IOPS" ] && [ "$IOPS" -gt 0 ] && ok || bad "iops: $IOPS"
echo "$R" | grep -q "wal: policy: fsync" && ok || bad "no policy line"

# the ceiling must not read as a sustained rate
echo "$R" | grep -q "UPPER BOUND" && ok \
	|| bad "policy states a rate without saying it is a bound: $R"
echo "$R" | grep -q "burst ceiling" && ok || bad "no burst ceiling wording"
echo "$R" | grep -qE "over [0-9]+ KB" && ok \
	|| bad "the ceiling cites no measured volume: $R"

# S63: no cache is written, and a second run measures again.  A cache
# keyed by device id survives a storage migration that changes nothing
# it can see, so it must not exist at all.
[ -e "$D/.pc-walprobe" ] && bad "a probe cache file was written" || ok
R=$("$BIN" -P "$D")
echo "$R" | grep -q "(cached)" && bad "second run served a cached result" || ok
echo "$R" | grep -q "wal: probe dev" && ok || bad "second run did not measure"

# daemon startup wiring: probe=no reports skipped, auto reports numbers
mkconf() { # mkconf <probe-mode> <dir>
	cat > "$D/w.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = wp-client-secret
cluster = wp-cluster-secret
[listen]
tcp = 127.0.0.1:16484
plaintext = loopback
[wal]
dir = $2
probe = $1
[collection th]
buckets_log2 = 10
EOF
}
startstop() {
	"$BIN" -f "$D/w.conf" > "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/log" && break
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
}

mkconf no "$D"
startstop
grep -q "wal: probe skipped" "$D/log" && ok || bad "probe=no not skipped"
grep -q "storage: mount" "$D/log" && ok || bad "identity missing at startup"

mkconf auto "$D"
startstop
grep -q "(cached)" "$D/log" && bad "startup served a cached probe" || ok
grep -q "wal: probe dev" "$D/log" && ok || bad "startup did not measure"
grep -q "wal: policy: fsync" "$D/log" && ok || bad "startup policy missing"
[ -e "$D/.pc-walprobe" ] && bad "startup wrote a probe cache" || ok

# RED: a nonexistent wal dir refuses startup
mkconf auto /no/such/wal
"$BIN" -f "$D/w.conf" > "$D/log2" 2>&1
[ $? -ne 0 ] && grep -q "unusable" "$D/log2" && ok \
	|| bad "nonexistent wal dir accepted"

echo "walprobetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
