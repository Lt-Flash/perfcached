#!/bin/sh
# containers-up.sh - three perfcached:debian nodes and redis:8 on one bridge network (docker or
# nerdctl); glibc, as musl's allocator serialises libperfd.  Usage: bench/containers-up.sh [arena_mb] [wal]
set -eu

ARENA=${1:-128}
WAL=${2:-nowal}
# IMAGE stands the same fleet up on another build: run against the binary that lacks a fix and
# watch it fail first
IMG=${IMAGE:-perfcached:debian}
RT=${RUNTIME:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
command -v "$RT" >/dev/null 2>&1 || { echo "no $RT in PATH"; exit 1; }
echo "runtime: $RT   arena: ${ARENA}MB   durability: $WAL   image: $IMG"

$RT network inspect pcnet >/dev/null 2>&1 || \
	$RT network create --subnet 10.99.0.0/24 pcnet >/dev/null
mkdir -p /var/tmp/pccluster

for i in 1 2 3; do
	WALSEC=""
	VOL=""
	if [ "$WAL" = wal ]; then
		mkdir -p /var/tmp/pccluster/wal$i
		# explicit sizing: the autosize probe once filled the host filesystem
		WALSEC="[wal]
dir = /wal
fsync = everysec
segment_mb = 64
segments = 8"
		VOL="-v /var/tmp/pccluster/wal$i:/wal"
	fi
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
$WALSEC
[collection b]
buckets_log2 = 18
pull = 1
[collection px]
buckets_log2 = 18
mode = proxy
[collection sh]
buckets_log2 = 16
mode = shard
EOF
	$RT rm -f pcnode$i >/dev/null 2>&1 || true
	# --ulimit memlock: the container default (64 KB) leaves the arena unpinned.  The config sits at
	# /etc/pcnode.conf, which perfcli does not search, so it never dials the loopback door encrypted.
	$RT run -d --name pcnode$i --network pcnet --ip 10.99.0.1$i \
		--ulimit memlock=-1:-1 \
		-v /var/tmp/pccluster/node$i.conf:/etc/pcnode.conf:ro \
		$VOL "$IMG" -f /etc/pcnode.conf -D >/dev/null
done

$RT rm -f pcredis >/dev/null 2>&1 || true
$RT run -d --name pcredis --network pcnet --ip 10.99.0.20 \
	redis:8 >/dev/null
if [ "$WAL" = wal ]; then
	$RT rm -f pcredis-aof >/dev/null 2>&1 || true
	mkdir -p /var/tmp/pccluster/aof
	$RT run -d --name pcredis-aof --network pcnet --ip 10.99.0.21 \
		-v /var/tmp/pccluster/aof:/data \
		redis:8 --appendonly yes >/dev/null
fi

# membership convergence: from now until every node reports peers_up=2
T0=$(date +%s%N)
i=0
while [ $i -lt 300 ]; do
	UP=0
	for n in 1 2 3; do
		P=$($RT exec pcnode$n perfcli -q -j '{"method":"stats"}' \
			2>/dev/null | python3 -c \
			'import json,sys
c=json.load(sys.stdin).get("cluster") or {}
print(1 if c.get("peers_up")==2 and c.get("role")!="joining" else 0)' \
			2>/dev/null || echo 0)
		UP=$((UP + P))
	done
	[ $UP -eq 3 ] && break
	sleep 0.2; i=$((i+1))
done
T1=$(date +%s%N)
if [ $UP -ne 3 ]; then
	# what each node says, and its log - a CI run shows nothing else
	echo "cluster did NOT converge in 60 s:"
	for n in 1 2 3; do
		echo "  pcnode$n: $($RT inspect -f '{{.State.Status}} exit={{.State.ExitCode}}' pcnode$n 2>&1)"
		$RT exec pcnode$n perfcli -q -j '{"method":"stats"}' 2>&1 | python3 -c \
			'import json,sys
try:
    r=json.load(sys.stdin); c=r.get("cluster") or {}
    print("    state=%s role=%s peers_up=%s node=%s master=%s" % (r.get("state"),
        c.get("role"), c.get("peers_up"), c.get("node"), c.get("master")))
except Exception as e:
    print("    stats unreadable: %s" % e)'
		$RT logs --tail 15 pcnode$n 2>&1 | sed 's/^/    /'
	done
	exit 1
fi
echo "membership converged $(( (T1 - T0) / 1000000 )) ms after the last container started"
for n in 1 2 3; do
	$RT exec pcnode$n perfcli -q -j '{"method":"stats"}' | python3 -c \
		'import json,sys
c=json.load(sys.stdin)["cluster"]
print("  pcnode: id=%s role=%s master=%s peers_up=%s" %
      (c["node"], c["role"], c["master"], c["peers_up"]))'
done
echo "up: pcnode1-3 (10.99.0.11-13) + pcredis (10.99.0.20)$([ "$WAL" = wal ] && echo ' + pcredis-aof (10.99.0.21)')"
echo "next: bench/run-matrix.sh"
