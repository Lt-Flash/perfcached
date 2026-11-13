#!/bin/bash
# rig.sh - requests per syscall on the RESP door, perfcached against redis-server, one host, perf
# stat on each server under redis-benchmark; cleans up on exit.  Run as: ssh root@<host> bash -s < rig.sh
set -u
R=/tmp/om3
cd $R || exit 1
cleanup() {
	[ -f $R/pc.pid ] && kill "$(cat $R/pc.pid)" 2>/dev/null
	redis-cli -p 30300 shutdown nosave >/dev/null 2>&1
	sleep 1
	echo "left: $(pgrep -c -x perfcached) perfcached, $(pgrep -c -f 'redis-server 127.0.0.1:30300') redis"
	cd /; rm -rf $R
}
trap cleanup EXIT
cat > $R/c.conf <<C
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 1024
[secrets]
client = om3-client-secret
[listen]
tcp = 127.0.0.1:30202
resp = 127.0.0.1:30200
plaintext = loopback
[collection 0]
buckets_log2 = 20
C
chmod 600 $R/c.conf
taskset -c 0-4 ./perfcached -f $R/c.conf > $R/pc.log 2>&1 & echo $! > $R/pc.pid
k=0; until grep -q "perfcached ready" $R/pc.log || [ $k -ge 100 ]; do sleep 0.1; k=$((k+1)); done
taskset -c 0-4 redis-server --port 30300 --bind 127.0.0.1 --save '' --appendonly no --daemonize yes \
	--pidfile $R/r.pid --logfile $R/r.log --dir $R
k=0; until redis-cli -p 30300 ping >/dev/null 2>&1 || [ $k -ge 50 ]; do sleep 0.1; k=$((k+1)); done
echo "== $(./perfcached -V | head -1), io $(grep -o 'io: [a-z_]*' $R/pc.log | head -1); $(redis-server --version | cut -d' ' -f1-3)"
N=2000000
run() { # run <name> <pid> <port> <test> <P>
	perf stat -x, -e raw_syscalls:sys_enter,task-clock -p $2 -o $R/perf.out &
	pp=$!
	sleep 0.3
	out=$(taskset -c 5-9 redis-benchmark -h 127.0.0.1 -p $3 -t $4 -n $N -c 50 -P $5 -d 32 -r 1000000 \
		--threads 4 -q --csv 2>/dev/null | tail -1 | cut -d, -f2 | tr -d '"')
	kill -INT $pp; wait $pp 2>/dev/null
	sys=$(grep raw_syscalls $R/perf.out | cut -d, -f1); tc=$(grep task-clock $R/perf.out | cut -d, -f1)
	awk -v n=$1 -v t=$4 -v p=$5 -v rps="$out" -v s="$sys" -v c="$tc" -v N=$N 'BEGIN {
		printf "%-10s %-3s P%-2s %10.0f req/s %10d syscalls %6.2f req/syscall %6.3f us CPU/req\n",
			n, t, p, rps, s, N / s, c * 1000 / N }'
}
for round in 1 2; do
	for P in 1 16; do
		for t in set get; do
			run perfcached "$(cat $R/pc.pid)" 30200 $t $P
			run redis "$(cat $R/r.pid)" 30300 $t $P
		done
	done
done
echo "== perfcached log: $(grep -c -E 'ERROR|CRIT' $R/pc.log) ERROR/CRIT"
