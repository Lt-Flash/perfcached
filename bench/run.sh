#!/bin/sh
# bench/run.sh — S10: single-node baselines, perfcached (text dialect,
# plaintext loopback) vs Redis, one client (pcbench) for both.
#
# Bench hygiene (the standing rules):
#   - stale bench processes killed first (bracket patterns);
#   - the build is verified OPTIMIZED (-O2 in the compile line) before
#     a single number is taken;
#   - every arm's server config is explicit below - nothing inherited;
#   - server pinned to cores 0-7, clients to 8-15 (16-vCPU host);
#   - each run refills+warms (2s discarded) - cold caches are not data;
#   - pcbench fails the arm loudly on any error reply.
#
# Usage: bench/run.sh [results-file]
set -eu

OUT=${1:-bench/results-$(date +%Y%m%d-%H%M).txt}
PPORT=16488
RPORT=16489
KEYS=${BENCH_KEYS:-100000}
VAL=${BENCH_VAL:-64}
SECS=${BENCH_SECS:-10}
D=$(mktemp -d)
trap 'pkill -f "perfcache[d] -f $D/" 2>/dev/null || true; \
      pkill -f "redis-serve[r] .*:$RPORT" 2>/dev/null || true; \
      rm -rf "$D"' EXIT

# ---- hygiene ---------------------------------------------------------------
pkill -f "perfcache[d] -f /tmp/bench" 2>/dev/null || true
pkill -f "redis-serve[r] .*:$RPORT" 2>/dev/null || true
# -nB: dry-run AS IF rebuilding, so the compile lines (and their -O2)
# are visible even when the binary is already up to date
make -nB perfcached | grep -q -- '-O2' || {
	echo "REFUSING: build is not -O2"; exit 1; }
make perfcached >/dev/null
cc -O2 -std=gnu11 -Wall -Wextra -Werror -o bench/pcbench bench/pcbench.c \
	-lpthread

say() { echo "$@" | tee -a "$OUT"; }
say "== pcbench baselines, $(hostname), $(date -u +%FT%TZ) =="
say "== keys=$KEYS val=${VAL}B secs=$SECS warm=2s; server cores 0-7, client 8-15 =="
say "== perfcached $(git rev-parse --short HEAD 2>/dev/null || echo '?'), $(redis-server --version | cut -d' ' -f3) =="

run() { # run <proto> <port> <conns> <depth> <getpct> <label>
	r=$(taskset -c 8-15 bench/pcbench -h 127.0.0.1 -p "$2" -P "$1" \
		-c "$3" -d "$4" -M "$5" -n $KEYS -v $VAL -T $SECS -w 2)
	say "$6: $r"
}

start_perf() { # start_perf <workers>
	cat > "$D/p.conf" <<EOF
[daemon]
workers = $1
log_level = info   # NOT warn: the ready NOTICE is the start gate
[memory]
arena_mb = 512
[secrets]
client = bench-client-secret
cluster = bench-cluster-secret
[listen]
tcp = 127.0.0.1:$PPORT
plaintext = loopback
[collection b]
buckets_log2 = 17
EOF
	taskset -c 0-7 ./perfcached -f "$D/p.conf" > "$D/p.log" 2>&1 &
	PPID_=$!
	i=0
	while [ $i -lt 50 ]; do
		grep -q "perfcached ready" "$D/p.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "perfcached did not start"; cat "$D/p.log"; exit 1
}

stop_perf() { kill -TERM $PPID_ 2>/dev/null; wait $PPID_ 2>/dev/null || true; }

# ---- perfcached, 4 workers -------------------------------------------------
start_perf 4
say "-- perfcached workers=4 --"
run perf $PPORT 4 1  100 "GET rtt   "
run perf $PPORT 4 1  0   "SET rtt   "
run perf $PPORT 8 32 100 "GET piped "
run perf $PPORT 8 32 0   "SET piped "
run perf $PPORT 8 32 90  "MIX 90/10 "
stop_perf

# ---- perfcached, 8 workers (the scaling arm) -------------------------------
start_perf 8
say "-- perfcached workers=8 --"
run perf $PPORT 8 32 100 "GET piped "
run perf $PPORT 16 32 100 "GET piped c16"
stop_perf

# ---- redis -----------------------------------------------------------------
taskset -c 0-7 redis-server --port $RPORT --bind 127.0.0.1 \
	--appendonly no --save '' --daemonize no > "$D/r.log" 2>&1 &
RPID=$!
sleep 1
say "-- redis (default single-threaded) --"
run resp $RPORT 4 1  100 "GET rtt   "
run resp $RPORT 4 1  0   "SET rtt   "
run resp $RPORT 8 32 100 "GET piped "
run resp $RPORT 8 32 0   "SET piped "
run resp $RPORT 8 32 90  "MIX 90/10 "
run resp $RPORT 16 32 100 "GET piped c16"
kill -TERM $RPID 2>/dev/null; wait $RPID 2>/dev/null || true

say "== done =="
echo "results in $OUT"
