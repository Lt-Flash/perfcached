#!/bin/sh
# cluster-up.sh [arena_mb] - the containerised comparison deployment: 3 perfcached nodes + redis:7 (nerdctl,
# CNI bridge, multicast); build its image amd64 EXPLICITLY - a multi-arch cache can serve an arm/v7 one.
set -eu
ARENA=${1:-128}
nerdctl network inspect pcnet >/dev/null 2>&1 || \
	nerdctl network create --subnet 10.99.0.0/24 pcnet
mkdir -p /var/tmp/pccluster
for i in 1 2 3; do
	cat > /var/tmp/pccluster/node$i.conf <<EOF
[daemon]
workers = 4
log_level = info
[memory]
arena_mb = $ARENA
[secrets]
client = bench-client-secret
cluster = bench-cluster-secret
[listen]
tcp = 127.0.0.1:6479
plaintext = loopback
[cluster]
multicast = 239.255.99.9:7191
advertise = 10.99.0.1$i
[collection b]
buckets_log2 = 18
[collection loc]
buckets_log2 = 18
[collection px]
buckets_log2 = 16
mode = proxy
EOF
	nerdctl rm -f pcnode$i >/dev/null 2>&1 || true
	nerdctl run -d --name pcnode$i --network pcnet --ip 10.99.0.1$i \
		-v /var/tmp/pccluster/node$i.conf:/etc/perfcached.conf:ro \
		localhost/perfcached:bench -f /etc/perfcached.conf >/dev/null
done
nerdctl rm -f pcredis >/dev/null 2>&1 || true
nerdctl run -d --name pcredis --network pcnet --ip 10.99.0.20 redis:7 >/dev/null
sleep 6
for i in 1 2 3; do
	nerdctl exec pcnode$i perfcli -q -j '{"method":"stats"}' | \
		python3 -c 'import json,sys; c=json.load(sys.stdin)["cluster"]; print("node: id=%s role=%s peers_up=%s" % (c["node"], c["role"], c["peers_up"]))'
done
