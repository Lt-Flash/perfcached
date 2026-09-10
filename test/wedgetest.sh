#!/bin/sh
# wedgetest.sh — a failed request must not disable the handle (S72).
#
# Starts one perfcached, lets the client connect and do a successful
# write, RESTARTS the daemon under it, and checks the client is still
# usable afterwards.  The failing request across the restart is
# expected; a handle that answers "pipeline in progress" forever is
# the defect.
#
# Usage: test/wedgetest.sh [./perfcached] [./wedgetest]
set -u
BIN=${1:-./perfcached}
WT=${2:-./wedgetest}
D=$(mktemp -d /var/tmp/pcwedge.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT TERM INT
SECRET=wedge-client-secret
ADDR=127.0.44.1
PORT=18440

cat > "$D/n.conf" <<EOC
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SECRET
cluster = wedge-cluster-secret
[listen]
tcp = $ADDR:$PORT
[collection w]
buckets_log2 = 12
EOC

start() {
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	PID=$!
	k=0
	while [ $k -lt 200 ]; do
		grep -q "perfcached ready" "$D/n.log" 2>/dev/null && return 0
		sleep 0.1; k=$((k + 1))
	done
	return 1
}

start || { echo "FAIL: daemon did not start"; tail -5 "$D/n.log"; exit 1; }

"$WT" "$ADDR" "$PORT" "$SECRET" w &
CLIENT=$!

# the client writes, then sleeps 8s; restart the daemon inside that window
sleep 3
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
: > "$D/n.log"
start || { echo "FAIL: daemon did not come back"; tail -5 "$D/n.log"; exit 1; }

wait $CLIENT
rc=$?
[ $rc -eq 0 ] && echo "wedgetest.sh: ok" || echo "wedgetest.sh: FAILED (rc=$rc)"
exit $rc
