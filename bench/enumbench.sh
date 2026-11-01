#!/bin/sh
# enumbench.sh — what enumeration costs, and what it costs EVERYONE ELSE.
#
# KEYS and SCAN are the operations S40 exists for: enumeration holds one
# worker, and the recorded finding (2026-08-30) is that a KEYS pass runs
# 4.3x slower wall-clock than the same walk offline - latency-only, no
# corruption.  No benchmark measured it, so the number could not move.
#
# This is a SEPARATE bench, not a containerbench arm, on purpose: the
# matrix there is throughput shapes across four cluster modes and takes
# the better part of an hour; enumeration is node-local and its
# interesting number is not throughput but COLLATERAL LATENCY - what
# happens to ordinary GETs while an enumeration runs.  One node per
# engine, minutes not hours, same image/network conventions as
# capacitybench.
#
# Three questions, per engine:
#   1. what does a full KEYS cost, wall-clock?
#   2. what does a full SCAN sweep cost, at the default COUNT and a
#      batched one?  (SCAN is the cooperative alternative KEYS is not.)
#   3. what happens to GET latency while each of those runs in a loop?
#
# THE HEADLINE IS EACH ENGINE AGAINST ITSELF - the degradation ratio,
# baseline GET vs GET-under-enumeration.  Redis is single-threaded and
# perfcached is not, so absolute cross-engine numbers mix threading
# models; the ratio does not.
#
# Usage:
#   bench/enumbench.sh
#   NKEYS=500000 bench/enumbench.sh        # a bigger dataset
#   ENGINES="perfcached" bench/enumbench.sh
set -u

# Runtime: RUNTIME=docker|podman|nerdctl, else autodetect.  The same
# scheme containerbench uses, for the same reason: defaulting to podman
# on a docker-only host died with "podman: not found" AFTER printing an
# unrelated build error.  Detection failures must be instant and name
# the fix.
RT=${RUNTIME:-}
if [ -n "$RT" ]; then
	command -v "$RT" >/dev/null 2>&1 || {
		echo "enumbench: RUNTIME=$RT is not in PATH"; exit 1; }
else
	for c in podman nerdctl docker; do
		command -v "$c" >/dev/null 2>&1 && { RT=$c; break; }
	done
	[ -n "$RT" ] || {
		echo "enumbench: no container runtime found (tried podman,"
		echo "nerdctl, docker).  Install one, or RUNTIME=<cmd> $0"
		exit 1; }
fi
# docker builds via BuildKit when available, legacy otherwise -
# containerbench's do_build, reduced
build_img() { # build_img <args...>
	if [ "$RT" = docker ]; then
		DOCKER_BUILDKIT=1 docker build "$@" 2>&1 ||
			DOCKER_BUILDKIT=0 docker build "$@" 2>&1
	else
		$RT build --platform linux/amd64 "$@" 2>&1
	fi
}
NET=${NET:-enumnet}
IMG=${IMG:-perfcached:enum}
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
SUBNET=${SUBNET:-10.90.0.0/24}
SECRET=enum-client-secret
ENGINES=${ENGINES:-"perfcached redis"}
NKEYS=${NKEYS:-100000}
VAL=${VAL:-200}
KEYS_REPS=${KEYS_REPS:-5}
SCAN_REPS=${SCAN_REPS:-3}
GET_N=${GET_N:-300000}                 # per GET arm; ~10-15s at P1
GET_CONNS=${GET_CONNS:-16}
# Table size for the perfcached collection.  17 gives 131072 buckets -
# the same density for 100k keys as redis's auto-resized dict, so SCAN
# sweeps compare like for like.  SCAN's COUNT is BUCKETS VISITED on
# both engines, so keys-per-call is COUNT x density: an oversized table
# (the old hardcoded 18) made every sweep pay 2x the round trips and
# read as "SCAN is 2x slower" when per-call cost was at parity.
BUCKETS_LOG2=${BUCKETS_LOG2:-17}
# GET arms run this many times; the MEDIAN is reported and the spread
# printed.  A single 12s sample declared "winners" inside its own noise
# - four back-to-back runs on one host crowned different engines on
# the near-parity rows.
GET_REPS=${GET_REPS:-3}
# NUMA pinning, applied to server, client and load containers alike.
# On a 2-node host an UNPINNED server lands its arena on whichever node
# the scheduler fancies, and the client lands near or far by draw - a
# per-run lottery measured as a monotonic-looking baseline drift.  Pin
# everything to one node for run-to-run comparability:
#   CPUSET_CPUS=0-3 CPUSET_MEMS=0 bench/enumbench.sh
CPUSET_CPUS=${CPUSET_CPUS:-}
CPUSET_MEMS=${CPUSET_MEMS:-}
PIN=""
[ -n "$CPUSET_CPUS" ] && PIN="$PIN --cpuset-cpus=$CPUSET_CPUS"
[ -n "$CPUSET_MEMS" ] && PIN="$PIN --cpuset-mems=$CPUSET_MEMS"
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
# Two workers is the COMPARISON default, not deployment advice: S45
# re-measured scaling at 5.8x by eight workers, so deploy to cores.
# Here W=2 keeps the engine-vs-itself degradation story conservative -
# with many workers a parked walk barely dents the aggregate and the
# stall ratio would flatter us.
WORKERS=${WORKERS:-2}
D=${D:-/var/tmp/enumbench}

say() { echo "$@" >&2; }

cleanup() {
	for c in enum-srv enum-cli enum-load; do
		$RT rm -f $c >/dev/null 2>&1
	done
	$RT network rm $NET >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

rm -rf "$D"; mkdir -p "$D"
REV=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
say "=== building $IMG (rev $REV) ==="
build_img -f bench/Containerfile.debian \
	--build-arg REV="$REV" -t "$IMG" . > "$D/build.log" 2>&1 || {
	say "image build FAILED ($RT) - see $D/build.log"
	tail -8 "$D/build.log" >&2; exit 1; }
cleanup
# docker's mtu option has a different name, and its --opt mtu= is
# SILENTLY IGNORED - containerbench found that out the hard way
case "$RT" in
docker) MTUOPT="com.docker.network.driver.mtu=9000" ;;
*)      MTUOPT="mtu=9000" ;;
esac
$RT network create --subnet "$SUBNET" --opt "$MTUOPT" $NET >/dev/null 2>&1 ||
	$RT network create --subnet "$SUBNET" $NET >/dev/null 2>&1 || {
	say "could not create $NET (subnet in use?)"; exit 1; }

SRV_IP=10.90.0.21
# one long-lived client container: exec into it for every measurement,
# so no arm pays container start-up inside its timed region
$RT run -d --name enum-cli --network $NET $PIN --entrypoint tail \
	"$REDIS_IMG" -f /dev/null >/dev/null 2>&1 || { say "client container failed"; exit 1; }

# EVERY exec is bounded.  A wedged server must fail an arm in seconds
# with a message, never sit silent until someone interrupts the bench.
rcli() { timeout 15 $RT exec enum-cli redis-cli -h $SRV_IP -p 6380 "$@"; }
# timed INSIDE the client container: one exec per measured region, GNU
# date at nanosecond resolution, N repetitions averaged
timed_ms() { # timed_ms <reps> <cmd...>  -> avg ms per repetition
	reps=$1; shift
	timeout 120 $RT exec enum-cli sh -c "
		s=\$(date +%s%N); i=0
		while [ \$i -lt $reps ]; do $* >/dev/null; i=\$((i+1)); done
		e=\$(date +%s%N); echo \$(( (e - s) / $reps / 1000000 ))"
}

INTEGRITY_FAIL=0
OUT=$D/results.tsv
: > "$OUT"
{
	echo "# enum arm  build=$REV date=$(date -u +%FT%TZ) host=$(hostname)" \
		"runtime=$RT nkeys=$NKEYS val=$VAL workers=$WORKERS(pc) buckets_log2=$BUCKETS_LOG2" \
		"get_conns=$GET_CONNS get_n=$GET_N"
	printf "engine\tmetric\tvalue\tunit\n"
} >> "$OUT"
row() { printf "%s\t%s\t%s\t%s\n" "$1" "$2" "$3" "$4" >> "$OUT"; }

# GET arm through redis-benchmark; parses rps + p50 + p99 from the
# latency summary line ("avg min p50 p95 p99 max")
get_arm() { # get_arm <engine> <label> - GET_REPS runs, median + spread
	ga_eng=$1; ga_lbl=$2
	ga_r=""; ga_9=""; ga_5=""
	ga_i=0
	while [ $ga_i -lt "$GET_REPS" ]; do
		ga_out=$(timeout 120 $RT exec enum-cli redis-benchmark \
			-h $SRV_IP -p 6380 -t get -r "$NKEYS" -n "$GET_N" \
			-c "$GET_CONNS" -d "$VAL" 2>&1 | tr '\r' '\n')
		ga_r="$ga_r $(echo "$ga_out" | \
			grep -oE "[0-9.]+ requests per second" | tail -1 | \
			cut -d' ' -f1)"
		ga_lat=$(echo "$ga_out" | grep -A2 "latency summary" | tail -1)
		ga_5="$ga_5 $(echo "$ga_lat" | awk '{print $3}')"
		ga_9="$ga_9 $(echo "$ga_lat" | awk '{print $5}')"
		ga_i=$((ga_i + 1))
	done
	set -- $(echo "$ga_r" | tr ' ' '\n' | grep . | sort -n | \
		awk '{v[NR]=$1} END{m=v[(NR+1)/2]; s=NR>1?100*(v[NR]-v[1])/m:0;
		printf "%s %.1f", m, s}')
	ga_rps=$1; ga_spread=$2
	ga_p50=$(echo "$ga_5" | tr ' ' '\n' | grep . | sort -n | \
		awk '{v[NR]=$1} END{print v[(NR+1)/2]}')
	ga_p99=$(echo "$ga_9" | tr ' ' '\n' | grep . | sort -n | \
		awk '{v[NR]=$1} END{print v[(NR+1)/2]}')
	row "$ga_eng" "get_rps_$ga_lbl" "${ga_rps:-0}" "ops/s ~${ga_spread}%"
	row "$ga_eng" "get_p50_$ga_lbl" "${ga_p50:-?}" "ms"
	row "$ga_eng" "get_p99_$ga_lbl" "${ga_p99:-?}" "ms"
	say "  GET $ga_lbl: ${ga_rps:-?}/s (spread ${ga_spread}% over $GET_REPS)" \
		"p50=${ga_p50:-?} p99=${ga_p99:-?}"
}

# the enumeration load: an endless loop in ITS OWN NAMED container, so
# stopping it is `podman rm -f enum-load` - never a process-pattern kill
enum_load() { # enum_load <shell-loop>
	$RT run -d --name enum-load --network $NET $PIN --entrypoint sh \
		"$REDIS_IMG" -c "$1" >/dev/null 2>&1
}

run_engine() { # run_engine <perfcached|redis>
	eng=$1
	say "=== engine: $eng ==="
	$RT rm -f enum-srv >/dev/null 2>&1
	if [ "$eng" = perfcached ]; then
		cat > "$D/pc.conf" <<CFG
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = $SECRET
# required even with no [cluster] section - the daemon refuses to start
# without one.  Third harness bitten by this; see clustermaptest.
cluster = enum-cluster-secret
[listen]
tcp = 0.0.0.0:6479
resp = 0.0.0.0:6380
resp_allow = $SUBNET
[collection 0]
buckets_log2 = $BUCKETS_LOG2
CFG
		$RT run -d --name enum-srv --network $NET --ip $SRV_IP \
			$PIN --ulimit memlock=-1 \
			-v "$D/pc.conf:/etc/perfcached.conf:ro" \
			"$IMG" >/dev/null 2>&1 || { say "  server failed"; return 1; }
	else
		$RT run -d --name enum-srv --network $NET --ip $SRV_IP \
			$PIN "$REDIS_IMG" redis-server --port 6380 --save '' \
			--appendonly no >/dev/null 2>&1 || \
			{ say "  server failed"; return 1; }
	fi
	if [ -n "$CPUSET_CPUS" ]; then
		got=$($RT inspect -f "{{.HostConfig.CpusetCpus}}" enum-srv \
			2>/dev/null)
		[ "$got" = "$CPUSET_CPUS" ] \
			&& say "  pinned to CPUs $got mems ${CPUSET_MEMS:-any}" \
			|| say "  *** NOT PINNED: asked $CPUSET_CPUS, runtime says '$got'"
	fi
	i=0
	while [ $i -lt 60 ]; do
		[ "$(rcli PING 2>/dev/null)" = "PONG" ] && break
		# a container that already EXITED will never answer: say so
		# now, with its log, instead of polling out the full budget
		if [ "$($RT inspect -f "{{.State.Running}}" enum-srv \
		        2>/dev/null)" != "true" ]; then
			say "  server container is not running - its log:"
			$RT logs --tail 6 enum-srv 2>&1 | sed "s/^/    /" >&2
			return 1
		fi
		sleep 0.5; i=$((i+1))
	done
	if [ "$(rcli PING 2>/dev/null)" != "PONG" ]; then
		say "  never answered PING - server log tail:"
		$RT logs --tail 4 enum-srv 2>&1 | sed "s/^/    /" >&2
		return 1
	fi

	# ---- fill: exactly NKEYS keys, pipelined, same path both engines
	say "  filling $NKEYS keys x ${VAL}B"
	timeout 180 $RT exec enum-cli sh -c "
		awk -v n=$NKEYS -v v=$VAL 'BEGIN {
			pad = sprintf(\"%*s\", v, \"\"); gsub(/ /, \"x\", pad)
			for (i = 0; i < n; i++)
				printf \"SET key:%012d %s\r\n\", i, pad
		}' | redis-cli -h $SRV_IP -p 6380 --pipe" > "$D/$eng.fill" 2>&1
	dbsz=$(rcli DBSIZE | tr -dc 0-9)
	row "$eng" "dbsize" "${dbsz:-0}" "keys"
	if [ "${dbsz:-0}" -ne "$NKEYS" ]; then
		say "  FILL SHORT: dbsize=$dbsz want=$NKEYS - rows below are"
		say "  not comparable; see $D/$eng.fill"
	fi

	# ---- 0. INTEGRITY, before any clock runs.  Every historical
	# keys_full row - months of x2-3 "wins" - timed a "-ERR reply too
	# large" TRUNCATION, not an answer: the timer never looked at what
	# came back.  An arm that cannot verify its reply must not report
	# a number (the instruments-false-green lesson, again).
	kfirst=$(rcli KEYS "*" 2>&1 | head -1 | cut -c1-40)
	kcount=$(rcli KEYS "*" 2>/dev/null | grep -c "key:")
	if [ "${kcount:-0}" -ne "$NKEYS" ]; then
		say "  KEYS INTEGRITY FAIL: got $kcount of $NKEYS"
		say "    first line: $kfirst"
		row "$eng" "keys_integrity" "FAIL:$kcount" "of $NKEYS"
		INTEGRITY_FAIL=1
	else
		row "$eng" "keys_integrity" "ok" "$NKEYS keys"
	fi
	scount=$(rcli --scan --count 1000 2>/dev/null | grep -c "key:")
	[ "${scount:-0}" -ne "$NKEYS" ] && {
		say "  SCAN INTEGRITY FAIL: swept $scount of $NKEYS"
		row "$eng" "scan_integrity" "FAIL:$scount" "of $NKEYS"
		INTEGRITY_FAIL=1; } || \
		row "$eng" "scan_integrity" "ok" "$NKEYS keys"
	# the arena's ACTUAL page tier: a run whose arena fell to 4K pages
	# is not comparable to one on hugepages, and nothing recorded it
	if [ "$eng" = perfcached ]; then
		tier=$($RT logs enum-srv 2>&1 | grep -o "huge-page arena.*" | \
			head -1 | tr -d "\r")
		say "  ${tier:-tier line not found}"
		row "$eng" "arena_tier" "${tier:-unknown}" "-"
	fi

	# ---- 1. KEYS wall-clock
	k_ms=$(timed_ms "$KEYS_REPS" redis-cli -h $SRV_IP -p 6380 KEYS "'*'")
	row "$eng" "keys_full" "$k_ms" "ms"
	say "  KEYS *: ${k_ms}ms avg of $KEYS_REPS"

	# ---- 2. SCAN sweeps (redis-cli --scan drives the cursor loop)
	s10=$(timed_ms "$SCAN_REPS" redis-cli -h $SRV_IP -p 6380 --scan)
	row "$eng" "scan_count10" "$s10" "ms"
	s1k=$(timed_ms "$SCAN_REPS" redis-cli -h $SRV_IP -p 6380 --scan \
		--count 1000)
	row "$eng" "scan_count1000" "$s1k" "ms"
	say "  SCAN sweep: count10=${s10}ms count1000=${s1k}ms"

	# ---- 3. GET latency: alone, under KEYS, under SCAN
	get_arm "$eng" baseline
	enum_load "while :; do redis-cli -h $SRV_IP -p 6380 KEYS '*' >/dev/null 2>&1; done"
	get_arm "$eng" under_keys
	$RT rm -f enum-load >/dev/null 2>&1
	enum_load "while :; do redis-cli -h $SRV_IP -p 6380 --scan --count 1000 >/dev/null 2>&1; done"
	get_arm "$eng" under_scan
	$RT rm -f enum-load >/dev/null 2>&1

	$RT rm -f enum-srv >/dev/null 2>&1
}

for e in $ENGINES; do
	run_engine "$e" || say "engine $e FAILED - its rows are partial"
done

say ""
say "=== results ($OUT) ==="
cat "$OUT"

# ---- the ratio table: milliseconds do not read, multipliers do -------
# One direction throughout: >1.0 means perfcached is better, for the
# latency rows as much as the throughput rows.  Only printed when both
# engines produced rows - a single-engine run has nothing to compare.
awk -F"\t" '
NR > 2 && NF >= 3 { v[$1 "." $2] = $3 }
function have(m) { return ("perfcached." m in v) && ("redis." m in v) }
function r2(m) { return v["redis." m] }
function pc(m) { return v["perfcached." m] }
function row(label, m, dir,   ratio) {
	if (!have(m)) return
	ratio = (dir == "lo") ? r2(m) / pc(m) : pc(m) / r2(m)
	printf "  %-22s %12s %12s   x%.2f%s\n", label, pc(m), r2(m), ratio,
		(ratio >= 1.10) ? "  <- perfcached" : \
		(ratio <= 0.90) ? "  <- redis" : "  ~ parity"
}
function held(eng, m) { return 100 * v[eng "." m] / v[eng ".get_rps_baseline"] }
END {
	if (!have("get_rps_baseline")) exit
	printf "\n=== perfcached vs redis  (>1.0 = perfcached better) ===\n"
	printf "  %-22s %12s %12s   %s\n", "metric", "perfcached", "redis", "vs redis"
	row("KEYS full (ms)",        "keys_full",          "lo")
	row("SCAN sweep c10 (ms)",   "scan_count10",       "lo")
	row("SCAN sweep c1000 (ms)", "scan_count1000",     "lo")
	row("GET baseline (ops/s)",  "get_rps_baseline",   "hi")
	row("GET under KEYS",        "get_rps_under_keys", "hi")
	row("GET under SCAN",        "get_rps_under_scan", "hi")
	row("GET p99 baseline (ms)", "get_p99_baseline",   "lo")
	row("GET p99 under KEYS",    "get_p99_under_keys", "lo")
	row("GET p99 under SCAN",    "get_p99_under_scan", "lo")
	printf "\n  throughput HELD under enumeration (the S40 stall):\n"
	printf "  %-22s %11.1f%% %11.1f%%\n", "under a KEYS loop",
		held("perfcached", "get_rps_under_keys"), held("redis", "get_rps_under_keys")
	printf "  %-22s %11.1f%% %11.1f%%\n", "under a SCAN loop",
		held("perfcached", "get_rps_under_scan"), held("redis", "get_rps_under_scan")
}' "$OUT" | tee -a "$OUT" >&2
say ""
say "Cross-engine absolutes mix threading models (redis is"
say "single-threaded here; perfcached runs $WORKERS workers) - the HELD"
say "percentages are the stall story, the ratio column is the cost story."
say "Winner arrows only outside a 10% parity band; each GET row carries"
say "its measured spread.  On NUMA hosts run pinned (CPUSET_CPUS/MEMS)"
say "or run-to-run baselines are a placement lottery."
if [ "$INTEGRITY_FAIL" -ne 0 ]; then
	say ""
	say "*** AN INTEGRITY CHECK FAILED - rows above are marked; the"
	say "*** affected engine's timings measure something OTHER than"
	say "*** the operation named.  Fix that before reading numbers."
	exit 1
fi
