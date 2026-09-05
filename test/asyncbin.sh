#!/bin/sh
# asyncbin.sh — runs asyncbintest against a real daemon (S28/S32).
# Usage: test/asyncbin.sh [./perfcached] [./asyncbintest]
set -u

BIN=${1:-./perfcached}
T=${2:-./asyncbintest}
SEC=abin-client-secret
D=$(mktemp -d /var/tmp/pcabin.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT

cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = $SEC
cluster = abin-cluster-secret
[listen]
tcp = 127.0.64.1:17964
[collection ab]
buckets_log2 = 12
EOF

"$BIN" -f "$D/n.conf" > "$D/log" 2>&1 &
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
if ! grep -q "perfcached ready" "$D/log" 2>/dev/null; then
	echo "asyncbin: daemon did not start:"; tail -3 "$D/log"; exit 1
fi

"$T" 127.0.64.1 17964 "$SEC" ab
