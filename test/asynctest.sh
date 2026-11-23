#!/bin/sh
# asynctest.sh - libperfd's event-loop surface under libevent: the daemon is restarted mid-load and the
# consumer must reconnect (Noise included) from inside its loop without losing in-flight requests.
set -u

BIN=${1:-./perfcached}
AT=${2:-./asynctest}
# a directory of THIS RUN's own: the script removes it, and concurrent CI runs share the filesystem
D=$(mktemp -d /var/tmp/pcasync.XXXXXX)
SECRET=at-client-secret
P1=

[ -x "$AT" ] || { echo "asynctest: $AT not built (libevent-dev missing?) - \
SKIPPED, and a skip is not a pass"; exit 0; }

rm -rf "$D"; mkdir -p "$D"
# daemons die by the pids this script started, each checked against its command line; the name
# pattern is a fallback, its bracket keeping pkill from matching the shell running it
PIDS=""
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" \
     2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

# SO_REUSEPORT means a leftover fleet would bind alongside this one and
# the kernel would split the traffic - refuse rather than measure that
if ss -ltn 2>/dev/null | grep -q ":17801[[:space:]]"; then
	echo "asynctest: port 17801 already bound:" >&2
	ss -ltnp 2>/dev/null | grep ":17801[[:space:]]" >&2
	exit 1
fi

cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = $SECRET
cluster = at-cluster-secret
[listen]
tcp = 127.0.0.1:17801
[collection c]
buckets_log2 = 14
EOF

start() {
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	PIDS="$PIDS $!"
	P1=$!
	i=0
	while [ $i -lt 120 ]; do
		grep -q "perfcached ready" "$D/n.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "asynctest: daemon did not start" >&2
	return 1
}

start || exit 1

# the consumer kills the daemon itself mid-load; this brings it back so the reconnect has something
# to reconnect TO
(
	i=0
	while [ $i -lt 200 ]; do
		pgrep -f "[p]erfcached -f $D" >/dev/null 2>&1 || {
			sleep 0.5
			"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
			PIDS="$PIDS $!"
			exit 0
		}
		sleep 0.1; i=$((i + 1))
	done
) &
REVIVE=$!

"$AT" 127.0.0.1 17801 "$SECRET" c
RC=$?

kill $REVIVE 2>/dev/null
exit $RC
