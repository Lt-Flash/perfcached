#!/bin/sh
# exectest.sh - perfd_exec beside the RESP door of one daemon (test/execcmp.c).
# Usage: test/exectest.sh [./perfcached] [./execcmp]
set -u
BIN=${1:-./perfcached}
EC=${2:-./execcmp}
D=$(mktemp -d /var/tmp/pcexec.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
cat > "$D/c.conf" <<CONF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = exec-test-secret
[listen]
tcp = 127.0.0.1:17058
plaintext = loopback
[collection 0]
buckets_log2 = 10
CONF
chmod 600 "$D/c.conf"
"$BIN" -f "$D/c.conf" > "$D/log" 2>&1 &
P=$!
i=0
while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/log"; do sleep 0.1; i=$((i+1)); done
"$EC" 17058
RC=$?
[ $RC = 0 ] || tail -20 "$D/log"
kill -TERM $P 2>/dev/null; wait 2>/dev/null; P=
exit $RC
