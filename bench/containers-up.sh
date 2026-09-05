#!/bin/sh
# containers-up.sh — the 4-container bench topology on one machine:
# three perfcached:debian nodes (automatic membership over multicast on
# the bridge) + one redis:8, all on a user-defined bridge network.
# Works with docker OR containerd/nerdctl - set RUNTIME, or it picks
# whichever is installed (nerdctl preferred).
#
# GLIBC, NOT MUSL, and this is not a preference.  musl's allocator
# serialises libperfd's per-reply malloc - a musl-built client peaks at
# TWO threads and then goes BACKWARDS - so a musl fleet measures the
# allocator instead of the daemon, and its behaviour is not the
# behaviour we ship.  musl stays a supported BUILD target for the
# portability matrix (tools/matrix.sh); it is never what a fleet runs.
# redis:8 rather than its musl tag for the same reason: the musl tag
# would drive the RESP arms with a musl client while the native arms
# are not.
#
#   bench/containers-up.sh [arena_mb] [wal]
#
# Node configs land in /var/tmp/pccluster/ and differ ONLY by the
# advertise IP - membership needs nothing else per node.  Collection
# `b` is STORE mode with pull (a local miss fetches from a peer and
# keeps the copy); collection `px` is PROXY mode (each key on exactly
# one node, fleet capacity = the sum of arenas); collection `sh` is
# SHARD mode, where ownership is computed rather than remembered - the
# one mode that MOVES keys when membership changes, which is what the
# reshard fault test (test/reshardtest.sh) needs.  With `wal`, every
# node gets fsync=everysec WAL + RDB on a bind mount, and a SECOND
# redis (pcredis-aof, --appendonly yes) joins as the durability-matched
# arm.  Prints the membership-convergence time before exiting.
set -eu

ARENA=${1:-128}
WAL=${2:-nowal}
# IMAGE exists so a suite can stand the SAME fleet up on a different
# build - which is how a fix is shown to fix something: run the test
# against the binary that lacks it and watch it fail first.
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
		# explicit sizing: the autosize probe once picked 8 x 256 MB
		# per node on a fast disk and filled the host filesystem
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
	# --ulimit memlock: the container default (64KB) makes the arena
	# mlock fail into a warning; unlimited keeps the arena pinned
	$RT run -d --name pcnode$i --network pcnet --ip 10.99.0.1$i \
		--ulimit memlock=-1:-1 \
		-v /var/tmp/pccluster/node$i.conf:/etc/perfcached.conf:ro \
		$VOL "$IMG" -f /etc/perfcached.conf -D >/dev/null
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

# membership convergence: elapsed from now until every node reports
# peers_up=2 (ids assigned, master elected)
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
[ $UP -eq 3 ] || { echo "cluster did NOT converge"; exit 1; }
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
