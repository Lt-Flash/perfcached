#!/bin/bash
# drive.sh <rounds> <Ns> <configs> - the overflow bench (DESIGN S458, 12oc OC-0), run ON the bench host.
# A 3-node eager fleet in podman (bridge pcnet), one collection "th" per cycle, a fresh fleet per cycle;
# n1 is driven (8 workers), n2/n3 are peers, pcbench shares n1's netns (plaintext = loopback).
# Each cycle twice: MODE=eager (a production fleet) and MODE=local (nothing replicates: a miss stays local).
# configs: name:buckets_log2:autoscale, e.g. fix9:9:off auto:9:auto big:22:off
# Arms: fill (serial inserts), hit d1/d16, miss d1/d16, mix95 d16, new16 (SETs over fresh keys, last).
# CPU sets by env (defaults: a 2-socket 40-thread host, n1 on node 1, clients on node 0); IMG from mkimage.sh.
# PERF=1: futex counts (perf trace -s) and a profile per arm - those numbers are not for the tables.
set -u
ROUNDS=$1 NS=$2 CONFIGS=$3
IMG=${IMG:?IMG=<image from mkimage.sh>}
K=$(cd "$(dirname "$0")" && pwd) R=${RESULTS:-/tmp/leglock}/${RUNID:-$(date +%m%d-%H%M)}
N1CPU=${N1CPU:-10-17,30-37} N2CPU=${N2CPU:-18,38} N3CPU=${N3CPU:-19,39} CLCPU=${CLCPU:-0-9,20-29}
T=${T:-10} W=${W:-2} C=${C:-32}
mkdir -p "$R"
exec > >(tee -a "$R/drive.log") 2>&1
cleanup() { podman rm -f pcl-cl pcl-n1 pcl-n2 pcl-n3 >/dev/null 2>&1; }
trap cleanup EXIT

say() { echo "$(date +%T) $*"; }
stats() { podman exec "$1" perfcli -q -j '{"method":"stats"}' 2>/dev/null; }
cgcpu() {   # usage_usec of a container's cgroup
	local p; p=$(podman inspect -f '{{.State.CgroupPath}}' "$1")
	awk '/^usage_usec/{print $2}' "/sys/fs/cgroup$p/cpu.stat"
}
ready3() {  # every node ready with 2 peers up
	local i ok n
	for i in $(seq 1 300); do
		ok=0
		for n in 1 2 3; do
			stats pcl-n$n | python3 -c 'import json,sys
s=json.load(sys.stdin); c=s.get("cluster") or {}
sys.exit(0 if s.get("state")=="ready" and c.get("peers_up")==2 else 1)' 2>/dev/null && ok=$((ok+1))
		done
		[ $ok = 3 ] && return 0
		sleep 0.2
	done
	return 1
}
entries() { stats "$1" | python3 -c 'import json,sys
s=json.load(sys.stdin)
print(next((c.get("entries",0) for c in s.get("collections",[]) if c["name"]=="th"),-1))'; }
settle() {  # wait (60 s max) until both peers hold what n1 holds
	local want i e2 e3
	want=$(entries pcl-n1)
	for i in $(seq 1 300); do
		e2=$(entries pcl-n2); e3=$(entries pcl-n3)
		[ "$e2" = "$want" ] && [ "$e3" = "$want" ] && { echo "settled n1=$want"; return 0; }
		sleep 0.2
	done
	echo "NOT settled: n1=$want n2=$e2 n3=$e3"
}
up() {   # up <log2> <autoscale>
	podman network inspect pcnet >/dev/null 2>&1 || podman network create --subnet 10.99.0.0/24 pcnet >/dev/null
	cleanup
	local i cpu
	for i in 1 2 3; do
		WORKERS=$([ $i = 1 ] && echo 8 || echo 2) sh "$K/mkconf.sh" $i "$1" "$2" > "$R/n$i.conf"
		chmod 640 "$R/n$i.conf"
		cpu=$(eval echo \$N${i}CPU)
		podman run -d --name pcl-n$i --network pcnet --ip 10.99.0.1$i --ulimit memlock=-1:-1 \
			--security-opt seccomp=unconfined --cpuset-cpus "$cpu" --cpuset-mems "${SRVMEMS:-1}" \
			-v "$R/n$i.conf":/etc/pcnode.conf:ro "$IMG" -f /etc/pcnode.conf -D >/dev/null || return 1
	done
	ready3
}
bench() {   # bench <tag> <pcbench args...> - one timed run against n1, n1's CPU over the timed window
	local tag=$1; shift
	local c0 c1 t0 t1 out
	stats pcl-n1 > "$R/$cyc.$tag.pre.json"
	podman run --rm --name pcl-cl --network container:pcl-n1 --cpuset-cpus $CLCPU --cpuset-mems "${CLMEMS:-0}" \
		--entrypoint pcbench "$IMG" -h 127.0.0.1 -p 6479 -P bin -C th -v 200 "$@" > "$R/$cyc.$tag.out" 2>&1 &
	local pid=$!
	sleep $((W + 1))                   # podman start + the bench's own warm-up
	c0=$(cgcpu pcl-n1); t0=$(date +%s%N)
	if [ "${PERF:-0}" = 1 ]; then
		local hp; hp=$(podman inspect -f '{{.State.Pid}}' pcl-n1)
		perf trace -s -p "$hp" -o "$R/$cyc.$tag.trace" -- sleep 3 >/dev/null 2>&1
		perf record -q -F 1999 -p "$hp" -o "$R/$cyc.$tag.perf.data" -- sleep 3 >/dev/null 2>&1
	fi
	wait $pid
	c1=$(cgcpu pcl-n1); t1=$(date +%s%N)
	stats pcl-n1 > "$R/$cyc.$tag.post.json"
	out=$(grep -E "ops/s|FAILED|failed" "$R/$cyc.$tag.out" | tail -1)
	echo "$cyc $tag | $out | n1 cpu $(( (c1 - c0) / 1000 )) ms over $(( (t1 - t0) / 1000000 )) ms" | tee -a "$R/summary.txt"
}

say "preflight: $(uname -r) $(nproc) cpus; node1 $(cat /sys/devices/system/node/node1/cpulist); cpu10 sibling $(cat /sys/devices/system/cpu/cpu10/topology/thread_siblings_list)"
say "governor $(cat /sys/devices/system/cpu/cpu10/cpufreq/scaling_governor); load $(cut -d' ' -f1-3 /proc/loadavg)"
say "image $(podman run --rm --entrypoint perfcached $IMG -V | head -1)"
for r in $(seq 1 "$ROUNDS"); do
	set -- $CONFIGS
	k=$(( (r - 1) % $# )); cfgs="$(echo $CONFIGS | tr ' ' '\n' | tail -n +$((k + 1)) | tr '\n' ' ')$(echo $CONFIGS | tr ' ' '\n' | head -n $k | tr '\n' ' ')"
	modes=${MODES:-eager local}; [ -z "${MODES:-}" ] && [ $((r % 2)) = 0 ] && modes="local eager"
	for N in $NS; do
	for MODE in $modes; do
		export MODE
		for cf in $cfgs; do
			IFS=: read -r name lg as <<< "$cf"
			cyc="r$r.$MODE.$name.n$N"
			say "=== $cyc (buckets_log2 $lg, autoscale $as)"
			up "$lg" "$as" || { say "FLEET DID NOT COME UP"; for n in 1 2 3; do podman logs --tail 8 pcl-n$n; done; continue; }
			stats pcl-n1 > "$R/$cyc.start.json"
			t0=$(date +%s%N)
			podman run --rm --network container:pcl-n1 --cpuset-cpus $CLCPU --cpuset-mems "${CLMEMS:-0}" --entrypoint pcbench \
				"$IMG" -h 127.0.0.1 -p 6479 -P bin -C th -v 200 -c 1 -n "$N" -K thkey:01 -F > "$R/$cyc.fill.out" 2>&1
			t1=$(date +%s%N)
			echo "$cyc fill | $(cat "$R/$cyc.fill.out" | tail -1) | $(( (t1 - t0) / 1000000 )) ms" | tee -a "$R/summary.txt"
			[ $MODE = eager ] && settle
			for n in 1 2 3; do stats pcl-n$n > "$R/$cyc.filled.n$n.json"; done
			bench hit1   -c $C -d 1  -n "$N" -K thkey:01 -N -M 100 -T $T -w $W
			bench hit16  -c $C -d 16 -n "$N" -K thkey:01 -N -M 100 -T $T -w $W
			bench miss1  -c $C -d 1  -n "$N" -K nokey:01 -N -M 100 -T $T -w $W
			bench miss16 -c $C -d 16 -n "$N" -K nokey:01 -N -M 100 -T $T -w $W
			bench mix16  -c $C -d 16 -n "$N" -K thkey:01 -N -M 95  -T $T -w $W
			bench new16  -c $C -d 16 -n 2000000 -K newkey:1 -N -M 0 -T 5 -w 1
			for n in 1 2 3; do stats pcl-n$n > "$R/$cyc.end.n$n.json"; done
			for n in 1 2 3; do podman logs pcl-n$n > "$R/$cyc.n$n.log" 2>&1; done
			cleanup
		done
	done
	done
done
podman network rm pcnet >/dev/null 2>&1
say "DONE $R"
