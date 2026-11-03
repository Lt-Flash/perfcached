#!/bin/sh
# workerbaltest.sh - S460: libperfd moves its connections off a crowded daemon
# worker (see workerbaltest.c for the claims).  One standalone node, 8 workers,
# the encrypted door on loopback.
# Usage: test/workerbaltest.sh [./perfcached] [./workerbaltest]
set -u
BIN=${1:-./perfcached}
WB=${2:-./workerbaltest}
SEC=wb-client-secret
PORT=18461
D=$(mktemp -d /var/tmp/pcwb.XXXXXX)
PID=""
trap '[ -n "$PID" ] && grep -qa -- "$D" /proc/$PID/cmdline 2>/dev/null && kill -9 "$PID"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

cat > "$D/n.conf" <<EOF
[daemon]
workers = 8
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = $SEC
[listen]
tcp = 127.0.0.1:$PORT
[collection 0]
buckets_log2 = 10
EOF
chmod 600 "$D/n.conf"
mkdir -p "$D/state"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1
	i=$((i + 1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "FAIL daemon did not start"; tail -5 "$D/n.log"; exit 1; }
"$WB" 127.0.0.1 $PORT $SEC
rc=$?
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
PID=""
exit $rc
