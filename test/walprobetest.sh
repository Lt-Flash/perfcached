#!/bin/sh
# walprobetest.sh — S12 verification: the probe measures with the WAL's
# own I/O pattern, caches by device id, and the policy pairs measurement
# with identity honestly (a blazing tmpfs number must never read as
# durable).  Usage: test/walprobetest.sh [./perfcached]
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

# second run: the cache answers
R=$("$BIN" -P "$D")
echo "$R" | grep -q "wal: probe (cached)" && ok || bad "cache not used: $R"

# device-id validity: a corrupted cached id must force a re-probe
sed -i 's/^v1 [^ ]*/v1 0:0\/dead/' "$D/.pc-walprobe"
R=$("$BIN" -P "$D")
if echo "$R" | grep -q "wal: probe (cached)"; then
	bad "stale device id was trusted"
else ok; fi
R=$("$BIN" -P "$D")
echo "$R" | grep -q "wal: probe (cached)" && ok || bad "cache not repaired"

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
grep -q "wal: probe (cached)" "$D/log" && ok \
	|| bad "startup auto did not use the cache"
grep -q "wal: policy: fsync" "$D/log" && ok || bad "startup policy missing"

# RED: a nonexistent wal dir refuses startup
mkconf auto /no/such/wal
"$BIN" -f "$D/w.conf" > "$D/log2" 2>&1
[ $? -ne 0 ] && grep -q "unusable" "$D/log2" && ok \
	|| bad "nonexistent wal dir accepted"

echo "walprobetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
