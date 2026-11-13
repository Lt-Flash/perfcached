#!/bin/bash
# c2crig.sh - perf c2c on one perfcached (NUMA node 0) under redis-benchmark 32 B SET and GET from
# node 1, 10 s each; cleans up on exit.  Run as: ssh root@<host> bash -s < c2crig.sh
set -u
R=/tmp/om6
mkdir -p $R && cd $R || exit 1
cp /tmp/om3/perfcached $R/
cleanup() {
	[ -f $R/pc.pid ] && kill "$(cat $R/pc.pid)" 2>/dev/null
	sleep 2
	echo "left: $(pgrep -c -x perfcached) perfcached, $(pgrep -c -x redis-benchmark) benchmark"
	cd /; rm -rf $R /tmp/om3
}
trap cleanup EXIT
cat > $R/c.conf <<C
[daemon]
workers = 8
log_level = notice
[memory]
arena_mb = 2048
[secrets]
client = om6-client-secret
[listen]
tcp = 127.0.0.1:30502
resp = 127.0.0.1:30500
plaintext = loopback
[collection 0]
buckets_log2 = 20
C
chmod 600 $R/c.conf
taskset -c 0-9 ./perfcached -f $R/c.conf > $R/pc.log 2>&1 & echo $! > $R/pc.pid
k=0; until grep -q "perfcached ready" $R/pc.log || [ $k -ge 100 ]; do sleep 0.1; k=$((k+1)); done
P=$(cat $R/pc.pid)
for t in set get; do
	taskset -c 10-19 redis-benchmark -h 127.0.0.1 -p 30500 -t $t -n 100000000 -c 200 -P 16 -d 32 -r 1000000 \
		--threads 8 -q > $R/bm.$t 2>&1 &
	b=$!
	sleep 3
	perf c2c record -o $R/c2c.$t -p $P -- sleep 10 > /dev/null 2>&1
	kill $b 2>/dev/null; wait $b 2>/dev/null
	echo "== $t: $(tr '\r' '\n' < $R/bm.$t | grep -o '[0-9.]* requests per second' | tail -1)"
	perf c2c report -i $R/c2c.$t --stdio -NN --sort cl,symbol 2>/dev/null > $R/rep.$t
	sed -n '/Trace Event Information/,/Global Shared Cache Line/p' $R/rep.$t | grep -E 'Load HITM|Load Local HITM|Load Remote HITM|Total records|Store' | head -8
	echo "   -- the lines with the most HITMs (first 12 rows of the shared-line table):"
	sed -n '/Shared Data Cache Line Table/,/Shared Cache Line Distribution Pareto/p' $R/rep.$t | grep -E '^ +[0-9]+ ' | head -12 | cut -c1-200
	echo "   -- and their symbols:"
	sed -n '/Shared Cache Line Distribution Pareto/,$p' $R/rep.$t | grep -E '^ +[0-9.]+%|^  -+$|0x[0-9a-f]+ ' | grep -oE '[a-z_][a-z0-9_]+\+0x[0-9a-f]+|\[[.k]\] [a-zA-Z_][a-zA-Z0-9_]+' | sort | uniq -c | sort -rn | head -15
done
echo "== log: $(grep -c -E 'ERROR|CRIT' $R/pc.log) ERROR/CRIT"
