#!/bin/sh
# run-matrix.sh — the full 4-container measurement battery against a
# topology started by containers-up.sh.  Every leg runs INSIDE a
# container: perfcached legs over pcnode1's own loopback, redis legs
# from pcnode1 across the bridge veth (worth ~10-20us on a baseline;
# noted with the results, and irrelevant against the ms-scale effects
# the matrix exists to show).  See bench/CONTAINERS.md for what each
# leg measures and the results achieved.
#
#   bench/run-matrix.sh [results-file]
#
# Legs: store-mode latency/throughput (vs redis), CPU under load,
# KEYS/SCAN stall probes (vs redis), store-mode pull convergence,
# proxy-mode spread + rebalancer convergence timeline, memory census.
# If pcredis-aof is up (containers-up.sh ... wal), the durability legs
# run too.  RUN_RECOVERY=1 adds the kill -9 crash-recovery proof (it
# restarts pcnode2).
set -eu

OUT=${1:-bench/results-containers-$(date +%Y%m%d-%H%M).txt}
RT=${RUNTIME:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
KEYS=${BENCH_KEYS:-200000}
VAL=${BENCH_VAL:-256}
SECS=${BENCH_SECS:-10}
REDIS_IP=10.99.0.20

say() { echo "$@" | tee -a "$OUT"; }
x1() { $RT exec pcnode1 "$@"; }
pcb() { # pcb <node> <proto> <host> <port> <col> <conns> <depth> <getpct> <label> [extra-flag]
	r=$($RT exec pcnode$1 pcbench -h "$3" -p "$4" -P "$2" -C "$5" \
		-c "$6" -d "$7" -M "$8" -n $KEYS -v $VAL -T $SECS -w 2 -q ${10:-})
	say "$9: $r"
}
entries() { # entries <node> <col>
	$RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' | python3 -c \
		"import json,sys
print(next(c['entries'] for c in json.load(sys.stdin)['collections'] if c['name']=='$2'))"
}

say "== container matrix, $(hostname), $(date -u +%FT%TZ), runtime=$RT =="
say "== keys=$KEYS val=${VAL}B secs=$SECS warm=2s =="
say "== perfcached: $(x1 perfcached -V 2>&1 | head -1)"
say "== redis: $($RT exec pcredis redis-server --version)"
for n in 1 2 3; do
	$RT exec pcnode$n perfcli -q -j '{"method":"stats"}' | python3 -c \
		'import json,sys
c=json.load(sys.stdin)["cluster"]
print("   node id=%s role=%s peers_up=%s" % (c["node"],c["role"],c["peers_up"]))' \
		| tee -a "$OUT"
done

# ---- proxy mode: spread + rebalancer convergence timeline ---------------
say "-- proxy mode FIRST, on a clean fleet (no store-mode skew): fill $KEYS x ${VAL}B via pcnode1 --"
x1 pcbench -h 127.0.0.1 -p 6479 -P perf -C px -F -n $KEYS -v $VAL
T0=$(date +%s)
LAST=""
STABLE=0
while :; do
	E1=$(entries 1 px); E2=$(entries 2 px); E3=$(entries 3 px)
	NOW=$(( $(date +%s) - T0 ))
	say "   t=${NOW}s   px spread: $E1 / $E2 / $E3   (sum $((E1+E2+E3)))"
	CUR="$E1/$E2/$E3"
	[ "$CUR" = "$LAST" ] && STABLE=$((STABLE+1)) || STABLE=0
	LAST=$CUR
	[ $STABLE -ge 3 ] && break         # three identical 10s samples
	[ $NOW -ge 300 ] && { say "   (timeline cut at 300s)"; break; }
	sleep 10
done
say "-- proxy mode: the full leg battery through ONE ingress (node1) --"
# -N everywhere: the keyspace is already filled; a refill through a
# node with an empty locator would place DUPLICATE copies of
# remotely-held keys.  GET legs run FIRST: the reads warm the
# ingress's locator, so the SET legs that follow exercise the real
# forwarded-write plane (holder-serialized), not placement races.
pcb 1 perf 127.0.0.1 6479 px 4 1 100 "px GET rtt d1       " -N
pcb 2 perf 127.0.0.1 6479 px 4 1 100 "px GET d1 via node2 " -N
pcb 1 perf 127.0.0.1 6479 px 8 32 100 "px GET piped 8x32   " -N
pcb 1 perf 127.0.0.1 6479 px 4 1  0  "px SET rtt d1 (fwd) " -N
pcb 1 perf 127.0.0.1 6479 px 8 32 0  "px SET piped 8x32   " -N
pcb 1 perf 127.0.0.1 6479 px 8 32 90 "px MIX 90/10 8x32   " -N
say "   px spread after the write legs: $(entries 1 px) / $(entries 2 px) / $(entries 3 px)"

# ---- store mode: latency + throughput, perfcached vs redis --------------
say "-- store mode (col b), pcnode1 loopback --"
x1 pcbench -h 127.0.0.1 -p 6479 -P perf -C b -F -n $KEYS -v $VAL
pcb 1 perf 127.0.0.1 6479 b 4 1  100 "perf GET rtt d1     "
pcb 1 perf 127.0.0.1 6479 b 4 1  0   "perf SET rtt d1     "
pcb 1 perf 127.0.0.1 6479 b 8 32 100 "perf GET piped 8x32 "
pcb 1 perf 127.0.0.1 6479 b 8 32 0   "perf SET piped 8x32 "
pcb 1 perf 127.0.0.1 6479 b 8 32 90  "perf MIX 90/10 8x32 "
say "-- binary dialect (same daemon and keys, frames instead of JSON) --"
pcb 1 bin  127.0.0.1 6479 b 4 1  100 "bin  GET rtt d1     "
pcb 1 bin  127.0.0.1 6479 b 4 1  0   "bin  SET rtt d1     "
pcb 1 bin  127.0.0.1 6479 b 8 32 100 "bin  GET piped 8x32 "
pcb 1 bin  127.0.0.1 6479 b 8 32 0   "bin  SET piped 8x32 "
pcb 1 bin  127.0.0.1 6479 b 8 32 90  "bin  MIX 90/10 8x32 "
say "-- redis (from pcnode1, cross-veth ~+10-20us on baselines) --"
x1 pcbench -h $REDIS_IP -p 6379 -P resp -F -n $KEYS -v $VAL
pcb 1 resp $REDIS_IP 6379 b 4 1  100 "resp GET rtt d1     "
pcb 1 resp $REDIS_IP 6379 b 4 1  0   "resp SET rtt d1     "
pcb 1 resp $REDIS_IP 6379 b 8 32 100 "resp GET piped 8x32 "
pcb 1 resp $REDIS_IP 6379 b 8 32 0   "resp SET piped 8x32 "
pcb 1 resp $REDIS_IP 6379 b 8 32 90  "resp MIX 90/10 8x32 "

# ---- CPU under load (runtime stats sampled during a 30s 90/10 run) ------
say "-- CPU: idle sample, then 5 samples during 90/10 load on pcnode1 --"
$RT stats --no-stream --format '{{.Name}} cpu={{.CPUPerc}} mem={{.MemUsage}}' \
	| tee -a "$OUT"
x1 pcbench -h 127.0.0.1 -p 6479 -P perf -C b -c 8 -d 32 -M 90 \
	-n $KEYS -v $VAL -T 30 -w 1 -q > /tmp/pc-cpu-load.txt 2>&1 &
LOADPID=$!
sleep 4
for s in 1 2 3 4 5; do
	$RT stats --no-stream --format \
		'{{.Name}} cpu={{.CPUPerc}} mem={{.MemUsage}}' | tee -a "$OUT"
	sleep 3
done
wait $LOADPID || true
say "   load leg during sampling: $(cat /tmp/pc-cpu-load.txt)"
rm -f /tmp/pc-cpu-load.txt

# ---- the KEYS/SCAN stall probes (the production outage mechanism) -------
say "-- stall probes: sampler RTTs while a 2nd connection storms --"
say "· perfcached scan storm:"
x1 python3 /usr/local/bin/stallprobe.py perf 127.0.0.1 6479 b scan $KEYS | tee -a "$OUT"
say "· perfcached keys storm:"
x1 python3 /usr/local/bin/stallprobe.py perf 127.0.0.1 6479 b keys $KEYS | tee -a "$OUT"
say "· redis SCAN storm:"
x1 python3 /usr/local/bin/stallprobe.py resp $REDIS_IP 6379 b scan $KEYS | tee -a "$OUT"
say "· redis KEYS storm:"
x1 python3 /usr/local/bin/stallprobe.py resp $REDIS_IP 6379 b keys $KEYS | tee -a "$OUT"

# ---- store-mode pull convergence (read the set through a COLD node) -----
say "-- store-mode pull: pcnode2 reads col b cold (miss->pull->keep), then warm --"
say "   pcnode2 col b entries before: $(entries 2 b)"
r=$($RT exec pcnode2 pcbench -h 127.0.0.1 -p 6479 -P perf -C b \
	-c 4 -d 1 -M 100 -n $KEYS -v $VAL -T 5 -w 0 -q)
say "   cold pass (pull mix):  $r"
say "   pcnode2 col b entries after cold pass: $(entries 2 b)"
r=$($RT exec pcnode2 pcbench -h 127.0.0.1 -p 6479 -P perf -C b \
	-c 4 -d 1 -M 100 -n $KEYS -v $VAL -T 5 -w 0 -q)
say "   warm pass (local):     $r"

# ---- memory census ------------------------------------------------------
say "-- memory --"
for n in 1 2 3; do
	$RT exec pcnode$n perfcli -q -j '{"method":"stats"}' | python3 -c \
		"import json,sys
s=json.load(sys.stdin); m=s['memory']
print('   pcnode$n arena: total=%d used=%d live=%d   entries: %s' %
      (m['arena_total'], m['arena_used'], m['arena_live'],
       {c['name']: c['entries'] for c in s['collections']}))" | tee -a "$OUT"
done
say "   redis: $($RT exec pcredis redis-cli INFO memory | grep -E 'used_memory_human|used_memory_rss_human' | tr -d '\r' | tr '\n' ' ')"
$RT stats --no-stream --format '{{.Name}} rss={{.MemUsage}}' | tee -a "$OUT"

# ---- durability legs (only when the wal topology is up) -----------------
if $RT inspect pcredis-aof >/dev/null 2>&1; then
	say "-- durability arm (WAL everysec vs redis AOF everysec) --"
	x1 pcbench -h 10.99.0.21 -p 6379 -P resp -F -n $KEYS -v $VAL
	pcb 1 perf 127.0.0.1 6479 b 4 1  0 "perf SET d1 (WAL)   "
	pcb 1 perf 127.0.0.1 6479 b 8 32 0 "perf SET piped (WAL)"
	pcb 1 resp 10.99.0.21 6379 b 4 1  0 "aof  SET d1         "
	pcb 1 resp 10.99.0.21 6379 b 8 32 0 "aof  SET piped      "
	if [ "${RUN_RECOVERY:-0}" = 1 ]; then
		say "-- crash recovery: kill -9 pcnode2, restart, census --"
		# census the PROXY shard: placed/forwarded/migrated stores are
		# WAL-logged.  (Store-mode PULLED copies are passive and not
		# persisted by design - re-pullable from peers on miss.)
		B4=$(entries 2 px)
		$RT kill -s KILL pcnode2 >/dev/null
		T0=$(date +%s%N)
		$RT start pcnode2 >/dev/null
		i=0
		while [ $i -lt 600 ]; do
			$RT exec pcnode2 perfcli -q ping >/dev/null 2>&1 && break
			sleep 0.1; i=$((i+1))
		done
		T1=$(date +%s%N)
		say "   px entries before kill: $B4, after recovery: $(entries 2 px), recovery $(( (T1-T0)/1000000 )) ms"
	fi
fi

say "== matrix done, results in $OUT =="
