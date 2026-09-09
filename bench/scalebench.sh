#!/bin/sh
# scalebench.sh — throughput vs worker count, plus a perf profile at
# the top of the curve (S45).
#
# The daemon runs on the host pinned to half the CPUs and memtier runs
# in a container pinned to the other half, so the driver can neither be
# the ceiling nor share cachelines with what it measures.  The profile
# at the highest worker count is the only evidence admitted for "X is
# the serialization point" claims - this task retired a peaks-at-two
# "finding" that had survived a week past the build it described, and
# three contention theories died by reading before one survived by
# profile (the per-write Lamport tick - see DESIGN S45 for why it is
# load-bearing and cannot be batched away).
#
# Usage: bench/scalebench.sh    (on the build/test host, ~10 min)
set -u
cd "$(dirname "$0")/.." || exit 1
D=/var/tmp/s45; rm -rf $D; mkdir -p $D
IMG=docker.io/library/redis:8
MT=docker.io/redislabs/memtier_benchmark:latest

mkconf() { # mkconf <workers>
	cat > $D/pc.conf <<CFG
[daemon]
workers = $1
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = s45-client-secret
cluster = s45-cluster-secret
[listen]
tcp = 127.0.0.1:17551
resp = 127.0.0.1:17552
plaintext = loopback
[collection 0]
buckets_log2 = 17
CFG
	chmod 600 $D/pc.conf
}

echo "workers	op	ops_per_sec	p99_ms" > $D/curve.tsv
for W in 1 2 4 8; do
	for p in $(pgrep -x perfcached); do kill -9 $p 2>/dev/null; done
	sleep 1
	mkconf $W
	taskset -c 0-7 ./perfcached -f $D/pc.conf > $D/pc-w$W.log 2>&1 &
	sleep 2
	ss -ltn | grep -q 17552 || { echo "W=$W daemon not up"; continue; }
	# fill once per daemon
	timeout 120 podman run --rm --network=host --cpuset-cpus=8-15 $MT \
		-s 127.0.0.1 -p 17552 --key-minimum=1 --key-maximum=100000 \
		--data-size=200 --key-pattern=P:P --ratio=1:0 -t 2 -c 8 \
		-n 6250 --hide-histogram >/dev/null 2>&1
	for arm in "1:0 Set" "0:1 Get"; do
		ratio=${arm% *}; op=${arm#* }
		out=$(timeout 120 podman run --rm --network=host \
			--cpuset-cpus=8-15 $MT -s 127.0.0.1 -p 17552 \
			--key-minimum=1 --key-maximum=100000 --data-size=200 \
			--key-pattern=R:R --ratio=$ratio --pipeline=8 \
			-t 8 -c 8 --test-time=12 --hide-histogram \
			--distinct-client-seed 2>&1)
		line=$(echo "$out" | grep -E "^${op}s? " | tail -1)
		ops=$(echo "$line" | awk '{print $2}')
		p99=$(echo "$line" | awk '{print $7}')
		printf "%s\t%s\t%s\t%s\n" "$W" "$op" "${ops:-0}" "${p99:-?}" >> $D/curve.tsv
		echo "W=$W $op ${ops:-0}/s p99=${p99:-?}"
	done
done

echo ""
echo "=== the curve ==="
cat $D/curve.tsv

# ---- profile at W=8, GET then SET, 10s each ----
for p in $(pgrep -x perfcached); do kill -9 $p 2>/dev/null; done; sleep 1
mkconf 8
taskset -c 0-7 ./perfcached -f $D/pc.conf > $D/pc-prof.log 2>&1 &
sleep 2
PCPID=$(pgrep -x perfcached | head -1)
timeout 120 podman run --rm --network=host --cpuset-cpus=8-15 $MT \
	-s 127.0.0.1 -p 17552 --key-minimum=1 --key-maximum=100000 \
	--data-size=200 --key-pattern=P:P --ratio=1:0 -t 2 -c 8 -n 6250 \
	--hide-histogram >/dev/null 2>&1
for arm in "0:1 get" "1:0 set"; do
	ratio=${arm% *}; nm=${arm#* }
	podman run -d --name s45load --network=host --cpuset-cpus=8-15 $MT \
		-s 127.0.0.1 -p 17552 --key-minimum=1 --key-maximum=100000 \
		--data-size=200 --key-pattern=R:R --ratio=$ratio --pipeline=8 \
		-t 8 -c 8 --test-time=40 --hide-histogram >/dev/null 2>&1
	sleep 3
	perf record -g -p $PCPID -o $D/perf-$nm.data -- sleep 10 >/dev/null 2>&1
	podman rm -f s45load >/dev/null 2>&1
	echo ""
	echo "=== perf top, W=8 $nm ==="
	perf report -i $D/perf-$nm.data --stdio --no-children 2>/dev/null | \
		grep -E "^\s+[0-9]+\.[0-9]+%" | head -14
done
for p in $(pgrep -x perfcached); do kill -9 $p 2>/dev/null; done
echo "S45 BASELINE DONE"
