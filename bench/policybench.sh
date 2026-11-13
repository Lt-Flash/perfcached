#!/bin/sh
# policybench.sh [./perfcached] [./policybench] - the spreading policies side by side on a
# three-node fleet, the same clients and workload for each.
set -u
BIN=${1:-./perfcached}
PB=${2:-./policybench}
D=${POLICYBENCH_DIR:-/var/tmp/policybench}
CLIENTS=${CLIENTS:-12}
OPS=${OPS:-400}
SECRET=pb-client-secret
P1= P2= P3=
rm -rf "$D"; mkdir -p "$D"

# refuse to measure a build that cannot name itself: an unstamped binary's figures cannot be
# reproduced
BINREV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "POLICYBENCH: refusing to measure an unstamped build ($BINREV)" >&2
	exit 1
fi
echo "# build=$BINREV date=$(date -u +%Y-%m-%dT%H:%MZ) host=$(hostname) clients=$CLIENTS ops=$OPS"

trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done' EXIT TERM INT

i=1
while [ $i -le 3 ]; do
	cat > "$D/n$i.conf" <<CONF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = $SECRET
cluster = pb-cluster-secret
[listen]
tcp = 127.0.41.$i:$((18100 + i))
[cluster]
multicast = 239.255.77.131:17231
advertise = 127.0.41.$i
mode = store
collections = c
[collection c]
buckets_log2 = 14
CONF
	"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	eval "P$i=\$!"
	i=$((i + 1))
done
i=1
while [ $i -le 3 ]; do
	k=0
	while [ $k -lt 120 ]; do
		grep -q "perfcached ready" "$D/n$i.log" 2>/dev/null && break
		sleep 0.1; k=$((k + 1))
	done
	i=$((i + 1))
done
sleep 4

echo "# $CLIENTS independent clients, $OPS ops each, three-node store fleet"
for pol in failover rr leastconn weighted; do
	"$PB" 127.0.41.1 18101 "$SECRET" c "$pol" "$CLIENTS" "$OPS" \
		2>>"$D/err" || echo "policy=$pol FAILED"
done
