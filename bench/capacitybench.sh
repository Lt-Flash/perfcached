#!/bin/sh
# capacitybench.sh — S39, the capacity arm nobody has run.
#
# Every other benchmark in this repo measures a working set of a few MB
# against a 512 MB node - under 1% of ONE arena.  In that regime store
# and proxy keep everything local and shard is forced to spread for
# nothing, so shard measures slowest and the tables read as "shard is
# the slow mode".  That is an artefact of the workload: shard's whole
# purpose is capacity beyond one node, and no benchmark has ever entered
# the regime where that matters.
#
# This arm does.  Three nodes at ARENA MB each, and a working set that
# EXCEEDS one arena while fitting the fleet.
#
# THROUGHPUT IS NOT THE HEADLINE - HIT RATE IS.  A mode that has quietly
# stopped holding half the keyspace answers its misses FASTER than a
# mode that holds everything, so a throughput number taken while a mode
# is missing is not comparable to one taken while another is hitting.
# Every read row here carries the hit rate beside it.
#
# There is NO EVICTION.  pcache_htable.c returns "arena full - write
# dropped" and the client is told `cache full`; live records are never
# evicted to make room (reclaim only returns drained chunks to the OS).
# So store and eager do not degrade gracefully - they stop accepting
# writes, and the keys that never landed miss forever.  That also rules
# out redis-benchmark as the fill client: it aborts on the first -ERR,
# which would give the filling modes LESS fill effort than the others
# and silently rig the comparison.  natbench counts errors and carries
# on, so every mode gets the same wall-clock effort.
#
# Usage:
#   bench/capacitybench.sh
#   KEYSPACE=200000 bench/capacitybench.sh     # a bigger working set
#   MODEARMS="store shard" bench/capacitybench.sh
set -u

# RUNTIME=docker|podman|nerdctl, else autodetect - a docker-only host
# used to die with "podman: not found" (same fix as enumbench)
RT=${RUNTIME:-}
if [ -n "$RT" ]; then
	command -v "$RT" >/dev/null 2>&1 || {
		echo "capacitybench: RUNTIME=$RT is not in PATH"; exit 1; }
else
	for c in podman nerdctl docker; do
		command -v "$c" >/dev/null 2>&1 && { RT=$c; break; }
	done
	[ -n "$RT" ] || {
		echo "capacitybench: no container runtime found"; exit 1; }
fi
NET=${NET:-capnet}
IMG=${IMG:-perfcached:cap}
SUBNET=${SUBNET:-10.89.0.0/24}
SECRET=cap-client-secret
MODEARMS=${MODEARMS:-"store eager proxy shard"}
ARENA=${ARENA:-128}                    # PER NODE; the fleet is 3x this
VAL=${VAL:-1024}
# Sized from measurement, not from the value size: a 1024-byte value
# costs ~2212 bytes of arena_used (1536 live, and the allocator asks
# ~1.44x that again).  One 128 MB node therefore holds ~60,000 of them
# and the fleet ~182,000.  The default sits at ~2x one arena and ~2/3 of
# the fleet: comfortably past what one node can hold, comfortably inside
# what three can.  The run re-derives the real figure and prints it, so
# a wrong guess here is visible rather than assumed.
# 0 = CALIBRATE: fill one node until it refuses and size the working set
# from what it actually held.  A hardcoded number was wrong the moment
# arena_mb became a real bound (b1e71f1), and it would be wrong again the
# next time the record overhead moves.  The arm needs a working set that
# exceeds ONE node and fits THREE; only the daemon can say where that is.
KEYSPACE=${KEYSPACE:-0}
# multiple of one node's measured capacity to aim the working set at:
# past one arena, comfortably inside three.
WS_FACTOR=${WS_FACTOR:-2}
FILL_SECS=${FILL_SECS:-25}
READ_SECS=${READ_SECS:-10}
CONNS=${CONNS:-64}
DEPTH=${DEPTH:-32}
NCPU=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
THREADS=${THREADS:-$([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)}
D=${D:-/var/tmp/capacitybench}

say() { echo "$@" >&2; }
n_ip() { echo "10.89.0.2$1"; }

cleanup() {
	for c in cap-n1 cap-n2 cap-n3 cap-cli cap-cal; do
		$RT rm -f $c >/dev/null 2>&1
	done
	$RT network rm $NET >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

rm -rf "$D"; mkdir -p "$D"
say "=== building $IMG ==="
REV=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
[ "$RT" = docker ] && export DOCKER_BUILDKIT=1
$RT build --platform linux/amd64 -f bench/Containerfile.debian \
	--build-arg REV="$REV" -t "$IMG" . > "$D/build.log" 2>&1 || {
	say "image build FAILED - see $D/build.log"; tail -8 "$D/build.log" >&2; exit 1; }
cleanup
case "$RT" in
docker) MTUOPT="com.docker.network.driver.mtu=9000" ;;
*)      MTUOPT="mtu=9000" ;;
esac
$RT network create --subnet "$SUBNET" --opt "$MTUOPT" $NET >/dev/null 2>&1 ||
	$RT network create --subnet "$SUBNET" $NET >/dev/null 2>&1 || {
	say "could not create $NET (subnet in use?)"; exit 1; }
$RT run -d --name cap-cli --network $NET --entrypoint tail "$IMG" -f /dev/null \
	>/dev/null 2>&1 || { say "client container failed"; exit 1; }

mi() { # mi <node-ip> <method>
	$RT run --rm --network $NET --entrypoint perfcli "$IMG" \
		-h "$1" -p 6479 -a "$SECRET" -j "{\"method\":\"$2\"}" 2>/dev/null
}
field() { tr ',' '\n' | grep -o "\"$1\":[0-9]*" | head -1 | cut -d: -f2; }

start_fleet() { # start_fleet <mode-block>
	i=1
	while [ $i -le 3 ]; do
		cat > "$D/n$i.conf" <<CFG
[daemon]
workers = $([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)
log_level = err
[memory]
arena_mb = $ARENA
[secrets]
client = $SECRET
cluster = cap-cluster-secret
[listen]
tcp = 0.0.0.0:6479
resp = 0.0.0.0:6380
resp_allow = $SUBNET
[cluster]
multicast = 239.98.0.13:6483
advertise = $(n_ip $i)
pull_timeout_ms = 400
$1
collections = 0
[collection 0]
buckets_log2 = 18
CFG
		$RT run -d --name cap-n$i --network $NET --ip "$(n_ip $i)" \
			--ulimit memlock=-1 -v "$D/n$i.conf:/etc/perfcached.conf:ro" \
			"$IMG" >/dev/null 2>&1 || { say "  node $i failed to start"; return 1; }
		i=$((i + 1))
	done
	k=0
	while [ $k -lt 40 ]; do
		peers=$(mi "$(n_ip 1)" members | grep -o '"node"' | wc -l)
		[ "${peers:-0}" -ge 3 ] && break
		sleep 2; k=$((k + 2))
	done
	if [ "${peers:-0}" -lt 3 ]; then
		say "  MEMBERSHIP DID NOT FORM ($peers of 3) - skipping, a"
		say "  capacity number from a partial fleet is meaningless"
		return 1
	fi
	say "  membership formed in ${k}s"
	return 0
}

# spares=0, i.e. a NON-ROUTING client, and this is the whole arm.
# libperfd routes by key when it has standbys (S35), so with routing on
# the CLIENT does the distributing and every mode spreads across the
# fleet - the first run of this harness reported store holding 115,000
# keys over three nodes, which is exactly what store is supposed not to
# do.  Pinning to one node is what makes the DAEMON's placement the
# thing being measured.
#
# ROUTE=1 gives the contrast run: the recommended production setup,
# where a cluster-aware client removes the forward hop.  Do not compare
# a routed number with a pinned one - they are different experiments.
ROUTE=${ROUTE:-0}
SPARES=$([ "$ROUTE" = 1 ] && echo -1 || echo 0)
nat() { # nat <getpct> <secs> -> raw natbench output
	$RT exec cap-cli natbench "$(n_ip 1)" 6479 "$SECRET" 0 \
		"$CONNS" "$THREADS" "$DEPTH" "$2" "$KEYSPACE" "$VAL" "$1" 1 0 \
		"$SPARES" 2>&1
}

printf '# capacity arm  build=%s date=%s host=%s runtime=%s arena_mb=%s/node fleet_mb=%s keyspace=%s val=%s fill_s=%s read_s=%s conns=%s depth=%s threads=%s routing=%s\n' \
	"$REV" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$RT" "$ARENA" \
	"$((ARENA * 3))" "$KEYSPACE" "$VAL" "$FILL_SECS" "$READ_SECS" \
	"$CONNS" "$DEPTH" "$THREADS" "$ROUTE" > "$D/results.tsv"
printf 'mode\thit_pct\tkeys_held\tmax_node\tkeyspace\theld_pct\tfleet_used_mb\tfill_err\tread_ops\tbytes_per_rec\n' \
	>> "$D/results.tsv"

# ---- calibrate ------------------------------------------------------
if [ "$KEYSPACE" -eq 0 ]; then
	say "--- calibrating: how many keys does ONE $ARENA MB node hold? ---"
	cat > "$D/cal.conf" <<CFG
[daemon]
workers = 2
log_level = err
[memory]
arena_mb = $ARENA
[secrets]
client = $SECRET
cluster = cap-cluster-secret
[listen]
tcp = 0.0.0.0:6479
[collection 0]
buckets_log2 = 18
CFG
	$RT rm -f cap-cal >/dev/null 2>&1
	$RT run -d --name cap-cal --network $NET --ip "$(n_ip 9)" \
		--ulimit memlock=-1 -v "$D/cal.conf:/etc/perfcached.conf:ro" \
		"$IMG" >/dev/null 2>&1 || { say "calibration node failed"; exit 1; }
	k=0
	while [ $k -lt 40 ]; do
		$RT run --rm --network $NET --entrypoint perfcli "$IMG" \
			-h "$(n_ip 9)" -p 6479 -a "$SECRET" \
			-j '{"method":"ping"}' >/dev/null 2>&1 && break
		sleep 0.5; k=$((k + 1))
	done
	# write far more than it can hold; the ceiling stops it, not us
	$RT exec cap-cli natbench "$(n_ip 9)" 6479 "$SECRET" 0 32 4 32 \
		"$FILL_SECS" 100000000 "$VAL" 0 1 0 0 >/dev/null 2>&1
	calst=$(mi "$(n_ip 9)" stats)
	CAP1=$(echo "$calst" | field entries)
	held=$(echo "$calst" | field arena_held)
	mx=$(echo "$calst" | field arena_max)
	$RT rm -f cap-cal >/dev/null 2>&1
	if [ "${CAP1:-0}" -lt 1000 ]; then
		say "  calibration held only ${CAP1:-0} keys - refusing to size a"
		say "  working set from that.  Raise ARENA or lower VAL."
		exit 1
	fi
	KEYSPACE=$((CAP1 * WS_FACTOR))
	say "  one node holds $CAP1 keys of $VAL B"
	say "  ($((${held:-0} / 1048576)) MB held of $((${mx:-0} / 1048576)) MB max -"
	say "  the gap is index and slab slack, not free space)"
	say "  working set = ${WS_FACTOR}x that = $KEYSPACE keys"
fi

say ""
say "working set: $KEYSPACE keys x $VAL B = $((KEYSPACE * VAL / 1048576)) MB of values"
say "one node holds: ${CAP1:-?} keys.  fleet arena: 3 x $ARENA MB"
say ""

for M in $MODEARMS; do
	case $M in
	store)  BLK="mode = store" ;;
	eager)  BLK="mode = eager" ;;
	proxy)  BLK="mode = proxy" ;;
	shard)  BLK="mode = shard" ;;
	*) say "unknown mode '$M'"; continue ;;
	esac
	say "--- mode=$M ---"
	cleanup_nodes() { for c in cap-n1 cap-n2 cap-n3; do $RT rm -f $c >/dev/null 2>&1; done; }
	cleanup_nodes
	start_fleet "$BLK" || { cleanup_nodes; continue; }

	say "  filling for ${FILL_SECS}s (writes are REFUSED when an arena"
	say "  fills - there is no eviction - so errors here are the result,"
	say "  not a harness fault)"
	fo=$(nat 0 "$FILL_SECS")
	ferr=$(echo "$fo" | awk '/ops\/s/{for(i=1;i<=NF;i++) if($i ~ /^errors=/){sub("errors=","",$i); print $i}}')

	held=0; used=0; usedheld=0; maxnode=0; per=""
	for i in 1 2 3; do
		st=$(mi "$(n_ip $i)" stats)
		e=$(echo "$st" | field entries); u=$(echo "$st" | field arena_used)
		held=$((held + ${e:-0})); used=$((used + ${u:-0}))
		[ "${e:-0}" -gt 0 ] && usedheld=$((usedheld + ${u:-0}))
		# per-node, because the SUM hides the distribution and the
		# distribution is the entire question: "the fleet holds it"
		# and "one node holds it" sum identically.
		per="$per n$i=${e:-0}/$(( ${u:-0} / 1048576 ))MB"
		[ "${e:-0}" -gt "${maxnode:-0}" ] && maxnode=${e:-0}
	done
	say "  per node:$per"
	# per-record cost from the nodes that actually HOLD something.  The
	# fleet total includes the baseline arena of every idle node, so
	# dividing it by keys_held charged store for two nodes holding
	# nothing and reported 4,387 B/record against shard's 2,979 - a
	# difference that was entirely the denominator.
	bpr=$([ "${held:-0}" -gt 0 ] && echo $((usedheld / held)) || echo 0)

	say "  reading for ${READ_SECS}s"
	ro=$(nat 100 "$READ_SECS")
	rops=$(echo "$ro" | awk '/ops\/s/{print $1}')
	rsec=$(echo "$ro" | awk '/ops\/s/{for(i=1;i<=NF;i++) if($i=="over"){print $(i+1)}}' | tr -d 's')
	hits=$(echo "$ro" | awk '/reads returning/{print $NF}')
	tot=$(awk -v o="${rops:-0}" -v s="${rsec:-0}" 'BEGIN{printf "%.0f", o*s}')
	hp=$(awk -v h="${hits:-0}" -v t="${tot:-0}" 'BEGIN{printf "%.1f", (t>0)? 100*h/t : 0}')
	hpct=$(awk -v h="${held:-0}" -v k="$KEYSPACE" 'BEGIN{printf "%.1f", (k>0)? 100*h/k : 0}')

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$M" "$hp" \
		"$held" "$maxnode" "$KEYSPACE" "$hpct" \
		"$((used / 1048576))" "${ferr:-?}" "${rops:-0}" "$bpr" \
		>> "$D/results.tsv"
	# entries_sum is a SUM over nodes, so a replicating mode counts each
	# key once per copy: eager read 72% of the keyspace by that measure
	# while its hit rate said 50.1%, the same as store.  HIT RATE is the
	# capacity number; the sum is a distinct-key count only for modes
	# that put a key on exactly one node.
	say "  entries_sum $held (most on one node: $maxnode) of $KEYSPACE,"
	say "  fleet $((used / 1048576)) MB, fill errors ${ferr:-?},"
	say "  read ${rops:-0}/s at ${hp}% HIT RATE  <- the capacity number"
	cleanup_nodes
	sleep 3
done

say ""
say "=== results ($D/results.tsv) ==="
cat "$D/results.tsv" >&2
say ""
say "READ hit_pct FIRST.  A mode that stopped holding the keyspace"
say "answers its misses faster than one that holds it, so read_ops"
say "means nothing without the hit rate beside it - eager posted the"
say "HIGHEST throughput of the four while serving half the keyspace."
say ""
say "keys_held is a SUM over nodes and counts a replicated key once per"
say "copy, so it is a distinct-key count only for modes that put a key"
say "on exactly one node.  It read 72% for eager whose hit rate was"
say "50.1%.  max_node is what the fullest single node held."
