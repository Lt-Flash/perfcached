#!/bin/sh
# respbench.sh — the RESP door (task S33) under REAL Redis clients.
#
# resplistener.sh already proves the door works and that 200 clients can
# hold it open at once.  What it does not answer is what an unmodified
# Redis client gets for its trouble, so this drives redis-benchmark - the
# tool those clients' owners will use - against three arms measured
# identically:
#
#   redis   real redis-server, persistence off: the reference
#   perf1   one perfcached, NO cluster section: the same job, standalone
#   perf1c  one perfcached as a ONE-NODE CLUSTER: the cluster plane runs
#           with no peers, so perf1 -> perf1c prices it at rest
#   perf3-store   three nodes, pull-on-miss: a SET lands locally and is
#                 pushed NOWHERE
#   perf3-eager   three nodes, store + the background sweep that pushes
#                 every record to all peers - the arm that actually
#                 prices REPLICATION, which store does not
#   perf3-proxy   three nodes, one placed copy; non-holder writes forward
#   perf3-shard   three nodes, one COMPUTED owner; non-owner writes
#                 forward - and a RESP client cannot compute it
#
# All the perf3 arms are driven through NODE 1's RESP door, because that
# is the honest shape of the migration this exists for: a Redis client
# CANNOT route - no map, no owner hash, no way to learn one - so on proxy
# and shard every key that does not belong to node 1 is a forward, and
# that forward is what a migrating deployment actually pays.
#
# perf1 -> perf1c prices the cluster plane at rest; perf1c -> perf3-store
# prices two more peers joining it.  NEITHER prices replication: that is
# perf3-eager, and it is the only arm whose work continues after the
# drive returns, so it waits for the sweep to converge before teardown.
#
# Client counts run to 1000 because "a lot of Redis clients at once" is
# the reason the door exists, and a throughput number at c=4 says nothing
# about c=1000.  Pipelining is measured separately: perfcached parses as
# many complete commands as one read produced, so -P 16 is a different
# question from -c 16 and is asked separately.
#
# Also checked, because throughput on a cache that lost the data is not a
# result: a key SET through one node's RESP port is read back through
# EVERY other node's, and then through a survivor after the node the
# writer used is killed.
#
# CONTAINERISED 2026-09-12.  Nothing runs on the host: the perfcached
# fleet, redis 8 and both load generators are containers on their own
# bench network.  The host needs a container runtime and nothing else.
# The two positional arguments this used to take (a perfcached and a
# perfcli path) are gone - the binaries come from the image built out of
# this tree, and the revision is read back OUT of that image.
#
# usage: bench/respbench.sh
set -u

B=$(cd "$(dirname "$0")" && pwd)
. "$B/containerlib.sh"
cl_prog=respbench
IMG=${IMG:-perfcached:bench}
CONTAINERFILE=${CONTAINERFILE:-bench/Containerfile.debian}
NET=${NET:-pcrespnet}
SUBNET=${SUBNET:-10.96.0.0/24}
RB_CLI=rb-drive            # redis-benchmark + redis-cli, from the redis image
RB_TOOL=rb-tool            # perfcli, from ours
RCON=rb-redis
D=${RESPBENCH_DIR:-/var/tmp/respbench}
N=${N:-100000}
N_MAX=${N_MAX:-2000000}   # cap on N x pipeline per cell
VAL=${VAL:-200}
# one epoll thread per worker, so more workers than CPUs is contention,
# not parallelism.  Hardcoded at 4 until a 2-vCPU host showed 62% SYSTEM
# time and a 12.3 ms p99 against redis's 1.7 at the same throughput.
# Capped at 4 so a >=4-core host still matches the published numbers.
NCPU=$(nproc 2>/dev/null || echo 2)
WORKERS=${WORKERS:-$([ "$NCPU" -lt 4 ] && echo "$NCPU" || echo 4)}
KEYSPACE=${KEYSPACE:-20000}
CLIENTS=${CLIENTS:-"1 4 16 50 200 500 1000"}
PIPES=${PIPES:-"1 8 16 64"}

# PIPE_CLIENTS: the client count for the PIPELINED cells.  The depth-1
# sweep walks $CLIENTS; the pipeline sweep holds clients fixed and walks
# depth, so each table varies one axis at a time.
#
# That fixed count used to be a hardcoded 50, which SILENTLY IGNORED
# CLIENTS: asking for CLIENTS=500 PIPES=64 measured 50 clients at depth
# 64.  The column said 50, so it was not hidden - but it was not what
# was asked for either.
#
# Now: if CLIENTS names exactly one value that is plainly the
# concurrency you meant, so use it.  Otherwise 50, which keeps the
# default matrix and the figures quoted in the README.
PIPE_CLIENTS=${PIPE_CLIENTS:-$(set -- $CLIENTS; [ $# -eq 1 ] && echo "$1" || echo 50)}

# ---- the shard arm is driven by a CLUSTER-AWARE client -----------------
#
# perfcached publishes the Redis topology (CLUSTER INFO/NODES/SLOTS/SHARDS,
# S44/S49) with a Redis-compatible slot function, hash tags included, so a
# stock client can route to the owner itself.  Measuring shard mode with a
# NON-cluster client pointed at one node therefore measured the forwarding
# hop and nothing else: two thirds of keys landed on a node that did not
# own them.  Paired on one fleet, 50 clients, pipeline 64, grace expired:
# 257,301 SET/s unrouted against 2,659,574 routed, x10.3, with fwd_sent
# EXACTLY ZERO in the routed run.  Unrouted, shard was the slowest arm in
# the matrix; routed, it is the fastest.
#
# THE ARM'S MEANING CHANGED ON 2026-09-12.  Do not compare a routed
# perf3-shard row against the x0.28 rows in older result files.  Set
# SHARD_CLUSTER=0 to reproduce those.
SHARD_CLUSTER=${SHARD_CLUSTER:-1}
RB_CLUSTER=""                      # "--cluster" while a routed arm runs
CLNODES=3                          # the fleet the mode arms build

# A routed cell needs at least one connection per node: with fewer, a
# routing client could not be talking to every owner at once, and one
# benchmark per node would inflate the concurrency the cell claims to
# measure.  Those cells stay unrouted and say so - a quietly unrouted
# cell inside a routed arm is the kind of mixed row that gets quoted.
routed_ok() { # routed_ok <clients>
	[ -n "$RB_CLUSTER" ] || return 1
	if [ "$1" -lt "$CLNODES" ]; then
		echo "  c=$1 < $CLNODES owners: this cell is UNROUTED" >&2
		return 1
	fi
	return 0
}

SECRET=rb-client-secret
P1= P2= P3= PR=

# no host-tool checks: redis-benchmark and redis-cli come from the redis
# image and perfcli from ours, so "installed" is a property of the images
# rather than of whichever host this happens to run on - which is the
# whole point of the container route.
cl_runtime_pick || exit 1

rm -rf "$D"; mkdir -p "$D"

cleanup() {
	cl_rm "$RB_CLI" "$RB_TOOL" "$RCON" rb-n1 rb-n2 rb-n3
	[ "${NET_KEEP:-0}" = 1 ] || cl_net_down "$NET"
}
trap cleanup EXIT TERM INT

# 1000 concurrent clients needs the descriptors for them on BOTH sides;
# the default 1024 is close enough to the ask to trip it
ulimit -n 8192 2>/dev/null || true

# BUSY-HOST PRE-FLIGHT.  This machine both runs the benchmarks and IS
# the GitLab CI runner, so a bench started near a push measures the
# pipeline as well: on 2026-09-12 a clang-tidy job ran straight through
# the redis, perf1, perf1c and store arms of a full run and every one of
# those cells had to be binned.  xhostbench has refused a busy server
# host since it was written; this had no such guard.
#
# Match the WORK, never the runner DAEMON: gitlab-runner's own command
# line contains its builds directory, so a pattern like
# "gitlab-runner.*build" matches the permanently-idle daemon and can
# never clear - that mistake cost a 900 s wait before it was spotted.
# Container processes are host processes, so this sees them too.
BUSY_CHECK=${BUSY_CHECK:-1}
host_busy() {
	hb_n=0
	for hb_p in /proc/[0-9]*; do
		# comm, NOT cmdline: comm is the executable's name, so a shell
		# whose ARGV happens to mention redis-benchmark - the launcher,
		# an ssh command, this function's own caller - cannot match
		# itself.  Verified: matching cmdline reported 1 busy process
		# on a completely idle box, and the one it found was the shell
		# doing the asking.
		read -r hb_c < "$hb_p/comm" 2>/dev/null || continue
		case "$hb_c" in
		clang-tidy|cc1|cc1plus|redis-benchmark|natbench|pcbench|perfcached)
			hb_n=$((hb_n + 1)) ;;
		esac
	done
	echo $hb_n
}
if [ "$BUSY_CHECK" = 1 ]; then
	hb=$(host_busy)
	if [ "${hb:-0}" -gt 0 ]; then
		echo "RESPBENCH: $hb compile/benchmark process(es) already running on \
$(hostname) - this run would measure them too.  Wait for CI to finish, or \
BUSY_CHECK=0 to override." >&2
		ps -eo pid,comm,etime --no-headers 2>/dev/null |
			grep -E 'clang-tidy|cc1|redis-benchmark|natbench|pcbench' |
			head -6 | sed 's/^/    /' >&2
		exit 1
	fi
fi

# PRE-FLIGHT.  The host-port version of this check is gone with the host
# processes, but the hazard it guarded is not: perfcached listeners use
# SO_REUSEPORT, so a fleet left by an interrupted run would bind ALONGSIDE
# this one and the kernel would split traffic between two unrelated
# fleets - and a leftover redis would answer PING and have the whole
# reference arm measure somebody else's instance.  In containers each
# fleet owns its own network namespace, so the equivalent hazard is a
# container of the same NAME left behind: remove those by name, never by
# pattern, which would match the shell running the sweep.
cl_rm "$RB_CLI" "$RB_TOOL" "$RCON" rb-n1 rb-n2 rb-n3
cl_net_up "$NET" "$SUBNET" || exit 1
cl_image_build "$IMG" "$CONTAINERFILE" || exit 1
cl_client_up "$RB_CLI" "$NET" || exit 1
cl_client_up "$RB_TOOL" "$NET" "$IMG" || exit 1

# PROVENANCE, read AFTER the image exists.  The numbers are only worth as
# much as the build that produced them, so stamp the BINARY's revision -
# out of the image that will actually run, not the tree, which differs
# the moment an older image meets a newer checkout - and refuse one that
# cannot name itself.  An unstamped build is how a page ends up quoting
# figures nobody can reproduce.
BINREV=$(cl_image_rev "$IMG")
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "RESPBENCH: refusing to measure an unstamped build ($BINREV) - \
build from a git checkout so PC_BUILD_REV is set" >&2
	exit 1
fi

nodec() { echo "rb-n$1"; }                # the node's container
addr() { echo "10.96.0.$((10 + $1))"; }   # ... and its address on $NET
nport() { echo "$((17400 + $1))"; }       # native listener
rport() { echo "$((16400 + $1))"; }       # RESP door
REDIS_IP=10.96.0.10
REDIS_PORT=6379

# perfcli lives in OUR image, redis-benchmark in the redis one, so the
# harness keeps one persistent client container of each.  Both are on
# $NET, so what they measure is the bench network, not a bridge the
# servers are not on.
statj() { cl_exec "$RB_TOOL" perfcli -h "$(addr $1)" -p "$(nport $1)" \
	-a "$SECRET" -j '{"method":"stats"}' 2>/dev/null; }
jget() { python3 -c "import json,sys
try:
    d = json.load(sys.stdin)
    print($1)
except Exception:
    print('')" 2>/dev/null; }

start_perf() { # start_perf <nodes> [cluster-mode-lines | "none"]
	n=$1; MODEBLK=${2:-"mode = store"}; i=1
	# "none" = NO [cluster] section at all.  A one-node CLUSTER is not
	# a standalone daemon: it still multicasts heartbeats, runs the
	# membership tick and keeps peer state, so measuring it as "one
	# perfcached" prices the cluster plane into the baseline and hides
	# it.  The two are measured separately instead.
	if [ "$MODEBLK" = none ]; then
		CLUSTERBLK=""
	else
		CLUSTERBLK="[cluster]
multicast = 239.255.77.145:17245
pull_timeout_ms = 400
$MODEBLK
collections = 0"
	fi
	while [ $i -le $n ]; do
		# NOTE: no 'plaintext = loopback' here.  It makes a loopback
		# listener plaintext-EXCLUSIVE, and perfcli authenticates -
		# the RESP door is plaintext on its own regardless.
		cat > "$D/n$i.conf" <<EOF
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = $SECRET
cluster = rb-cluster-secret
[listen]
tcp = $(addr $i):$(nport $i)
resp = $(addr $i):$(rport $i)
# S33: the RESP dialect has no handshake, so a NON-LOOPBACK listener is
# REFUSED without an allow-list and the daemon will not start at all.
# The host-process version of this bench listened on 127.0.42.x, which
# is loopback and needed none; on a bench network it does.  Scoped to
# the bench subnet, so the containers are the only things that reach it.
resp_allow = $SUBNET
$CLUSTERBLK
$([ -n "$CLUSTERBLK" ] && echo "advertise = $(addr $i)")
[collection 0]
buckets_log2 = 17
EOF
		# --ulimit memlock=-1: a container's default memlock sits
		# far below the arena, so mlock() fails and the arena runs
		# UNPINNED and swappable - a property of the container, not
		# of the daemon, and invisible in the daemon's own log.
		CL_RUN_EXTRA="--ulimit memlock=-1"
		cl_node_up "$(nodec $i)" "$NET" "$(addr $i)" \
			"$D/n$i.conf" "$IMG" || exit 1
		CL_RUN_EXTRA=""
		eval "P$i=$(nodec $i)"
		i=$((i + 1))
	done
	i=1
	while [ $i -le $n ]; do
		cl_node_wait "$(nodec $i)" "$RB_TOOL" "$(addr $i)" \
			"$(nport $i)" "$SECRET" 120 || exit 1
		i=$((i + 1))
	done
	[ "$n" -gt 1 ] && sleep 4        # membership + election
	return 0
}
stop_perf() {
	cl_rm rb-n1 rb-n2 rb-n3
	P1= P2= P3=
	sleep 0.5
}
start_redis() {
	cl_redis_up "$RCON" "$NET" "$REDIS_IP" || return 1
	PR=$RCON
	return 0
}
stop_redis() { cl_rm "$RCON"; PR=; sleep 0.3; }

# one redis-benchmark point -> "set_rps get_rps set_p50 get_p50 set_p99 get_p99"
# NOTE: strip the quotes PER FIELD.  gsub(/"/,"") on $0 makes awk re-split
# the record with FS, and with FS='","' the unquoted line has nothing left
# to split on - every field but the first comes back empty and the point
# prints 0 as though it had measured one.
# Live ratio against the redis reference.  The redis arm always runs
# first, so by the time any other arm prints a line its comparison is
# already known - and a progress line with a bare number is close to
# useless while a run takes an hour.  The reference is kept in a file
# because POSIX sh has no associative arrays and the alternative is
# re-parsing the TSV per row.
ref_put() { # ref_put <clients> <pipeline> <set> <get>
	printf '%s %s %s %s\n' "$1" "$2" "$3" "$4" >> "$D/ref.txt"
}
ref_note() { # ref_note <clients> <pipeline> <set> <get> -> " (x.. / x..)"
	[ -r "$D/ref.txt" ] || return 0
	awk -v c="$1" -v p="$2" -v s="$3" -v g="$4" '
		$1 == c && $2 == p { rs = $3; rg = $4 }
		END {
			if (rs + 0 <= 0) exit
			if (s + 0 > 0 && g + 0 > 0)
				printf "  [SET x%.2f GET x%.2f vs redis]", s / rs, g / rg
			else if (g + 0 > 0)
				printf "  [GET x%.2f vs redis]", g / rg
		}' "$D/ref.txt"
}

# ---- tabular progress -------------------------------------------------
# One aligned table per arm instead of a stream of sentences: a run
# prints ~80 measurement lines and comparing them by eye across a
# scrolling terminal is not reading, it is squinting.  The header
# reprints whenever the arm changes, so a line always has a column
# heading above it no matter where you scroll in.
TROW_ARM=""
trow() { # trow <arm> <clients> <pipe> <set> <get> <sp99> <gp99>
	if [ "$1" != "$TROW_ARM" ]; then
		TROW_ARM=$1
		printf "\n  === %s ===\n" "$1" >&2
		printf "  %6s %5s %12s %12s %9s %9s  %s\n" \
			"cli" "pipe" "SET/s" "GET/s" "SET p99" "GET p99" \
			"vs redis" >&2
		printf "  %6s %5s %12s %12s %9s %9s  %s\n" \
			"-----" "----" "-----------" "-----------" \
			"--------" "--------" "--------------------" >&2
	fi
	# the reference arm is not compared with itself: printing x1.00
	# down the redis column is noise that reads like a measurement
	if [ "$1" = redis ]; then rn=""; else
		rn=$(ref_note "$2" "$3" "${4:-0}" "${5:-0}"); fi
	printf "  %6s %5s %12s %12s %9s %9s  %s\n" \
		"$2" "$3" "${4:--}" "${5:--}" "${6:--}" "${7:--}" "$rn" >&2
}

# Requests per cell scale with the pipeline depth.  redis-benchmark
# times a run in WHOLE MILLISECONDS and reports N / elapsed, so a
# 100k-request run at 1.8M ops/s lasts 56 ms and every figure it can
# print sits on a 2% grid - the same GET/s to the digit, run after run,
# was that grid and not repeatability.  N x P, capped at N_MAX, keeps
# every cell above a second without a lone connection at depth 64
# running for a minute.
reqs() { r=$((N * $1)); [ "$r" -gt "$N_MAX" ] && r=$N_MAX; echo "$r"; }
# Why a redis-benchmark call failed.  point() and point_get() used to send
# its stderr to /dev/null, so every failure - a refused connection, an error
# reply, a timeout - collapsed into "produced no result" and the cause was
# lost.  One run on 2026-09-11 dropped every pipelined cell of the headline
# arm that way and nobody could say why.  Each call now leaves its exit
# code, its stderr and its stdout in $D, overwritten per call (the harness
# runs one call at a time), and every call is also appended to
# $D/redis-benchmark.log.
#
# redis-benchmark ALWAYS prints "WARNING: Could not fetch server CONFIG"
# against perfcached's RESP door, which has no CONFIG verb.  That line is
# expected on every call and is never the cause, so it is filtered out.
rb_errs() {
	awk '!/Could not fetch server CONFIG/ && NF' "$D/rb.err" 2>/dev/null
}

rb_log() { # rb_log <arm> <clients> <pipeline> <rep>
	printf '%s c=%s P=%s rep=%s rc=%s\n' "$1" "$2" "$3" "$4" \
		"$(cat "$D/rb.rc" 2>/dev/null)" >> "$D/redis-benchmark.log"
	rb_errs | sed 's/^/    /' >> "$D/redis-benchmark.log"
}

rb_why() { # after a call that produced no result: say why, to stderr
	rb_rc=$(cat "$D/rb.rc" 2>/dev/null)
	rb_e=$(rb_errs | head -3)
	if [ -n "$rb_e" ]; then
		echo "  redis-benchmark exit ${rb_rc:-?}:" >&2
		printf '%s\n' "$rb_e" | sed 's/^/    /' >&2
	elif [ "${rb_rc:-1}" != 0 ]; then
		echo "  redis-benchmark exit ${rb_rc:-?}, with no error text" >&2
	else
		echo "  redis-benchmark exit 0 with no error - its output lacked" \
			"the SET or GET row this harness parses:" >&2
		sed 's/^/    /' "$D/rb.out" 2>/dev/null | head -4 >&2
	fi
}

point() { # point <host> <port> <clients> <pipeline>
	{ cl_bench "$RB_CLI" -h "$1" -p "$2" -t set,get -n "$(reqs "$4")" -c "$3" -P "$4" \
		-r "$KEYSPACE" -d "$VAL" --csv 2>"$D/rb.err"; echo $? > "$D/rb.rc"; } \
	| tee "$D/rb.out" \
	| awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) == "SET" { sr = unq($2); sp50 = unq($5); sp99 = unq($7) }
		unq($1) == "GET" { gr = unq($2); gp50 = unq($5); gp99 = unq($7) }
		END {
			if (sr == "" || gr == "")
				exit 1
			printf "%.0f %.0f %s %s %s %s\n", sr, gr, sp50, gp50,
				sp99, gp99
		}'
}

# ---- the ROUTED driver, for the shard arm ------------------------------
#
# redis-benchmark's own cluster mode CANNOT measure reads.  It spreads
# writes by randomising the HASH TAG per request - the keys it wrote in
# one run look like key:{4JO}:0, key:{EU}}:1, key:{49k}:4 - so its GET
# phase generates fresh tags and asks for keys its SET phase never wrote.
# No value of -r fixes that; the randomness is in the tag, not the
# suffix.  Measured: -r 20000 and -r 1000 both give ~43-55k GET/s with
# ~2 cluster pulls per request, which is the MISS path, not reads.
#
# So the routed arm models a cluster-aware client directly: one
# redis-benchmark per node, pinned to that node, using an explicit
# command whose key carries a hash tag THAT NODE OWNS, run in parallel
# and summed.  Every request therefore lands on its owner exactly as a
# routing client would send it, and the GET phase reads the keys the SET
# phase just wrote.  Verified: 1,658,374 GET/s on one node with 216
# pulls out of 2,000,000 requests - 0.01%, i.e. local hits.
#
# Sum the rates (that is the fleet's aggregate, which is what a routed
# client sees) and take the WORST p99 of the three, because a client is
# as slow as the node it is waiting on.
RVAL=""                            # filled once VAL is known
routed_tags() {                    # find a hash tag each node owns
	rt_i=1
	while [ $rt_i -le $CLNODES ]; do
		rt_found=""
		for rt_t in t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 ta tb tc td te tf; do
			rt_s=$(cl_exec "$RB_CLI" redis-cli -h "$(addr 1)" \
				-p "$(rport 1)" CLUSTER KEYSLOT "key:{$rt_t}:0" 2>/dev/null)
			[ -n "$rt_s" ] || continue
			rt_o=$(cl_exec "$RB_CLI" redis-cli -h "$(addr 1)" -p "$(rport 1)" \
				CLUSTER NODES 2>/dev/null |
				awk -v s="$rt_s" '{ip=$2; sub(/:.*/,"",ip);
					for(i=9;i<=NF;i++){split($i,r,"-"); lo=r[1];
					hi=(r[2]==""?r[1]:r[2]);
					if(s+0>=lo+0 && s+0<=hi+0){print ip; exit}}}')
			if [ "$rt_o" = "$(addr $rt_i)" ]; then rt_found=$rt_t; break; fi
		done
		[ -n "$rt_found" ] || {
			echo "RESPBENCH: no hash tag maps to $(addr $rt_i) - the routed" \
				"arm cannot be measured, falling back to unrouted" >&2
			RB_CLUSTER=""; return 1; }
		eval "RTAG$rt_i=\$rt_found"
		rt_i=$((rt_i + 1))
	done
	RVAL=$(python3 -c "print('x' * $VAL)")
	return 0
}

point_routed() { # point_routed <clients> <pipeline> -> the six fields
	pr_n=$(reqs "$2")
	pr_c=$(( $1 / CLNODES )); [ "$pr_c" -lt 1 ] && pr_c=1
	pr_i=1
	while [ $pr_i -le $CLNODES ]; do
		eval "pr_tag=\$RTAG$pr_i"
		(
			cl_bench "$RB_CLI" -h "$(addr $pr_i)" -p "$(rport $pr_i)" \
				-n "$pr_n" -c "$pr_c" -P "$2" -r "$KEYSPACE" --csv \
				SET "key:{$pr_tag}:__rand_int__" "$RVAL" \
				> "$D/rr-s$pr_i" 2>/dev/null
			cl_bench "$RB_CLI" -h "$(addr $pr_i)" -p "$(rport $pr_i)" \
				-n "$pr_n" -c "$pr_c" -P "$2" -r "$KEYSPACE" --csv \
				GET "key:{$pr_tag}:__rand_int__" \
				> "$D/rr-g$pr_i" 2>/dev/null
		) &
		pr_i=$((pr_i + 1))
	done
	wait
	cat "$D"/rr-s* "$D"/rr-g* 2>/dev/null | awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) ~ /^SET/ { sr += unq($2); if (unq($5)+0 > sp50) sp50 = unq($5);
		                   if (unq($7)+0 > sp99) sp99 = unq($7) }
		unq($1) ~ /^GET/ { gr += unq($2); if (unq($5)+0 > gp50) gp50 = unq($5);
		                   if (unq($7)+0 > gp99) gp99 = unq($7) }
		END {
			if (sr == 0 || gr == 0) exit 1
			printf "%.0f %.0f %s %s %s %s\n", sr, gr, sp50, gp50, sp99, gp99
		}'
}

# REPS: how many times each cell is measured before it is believed.
# Default 1 - a single pass is enough for the redis/perf1 headline,
# where the gap is large.  It is NOT enough to COMPARE ARMS: this rig's
# run-to-run spread on one unchanged arm is ~11%, and the mode arms
# differ by 14-20%, so a single pass would be ranking noise - and the
# REFERENCE arm is no better: redis itself moved 411k -> 571k between two
# runs of the same cell here, 39%.  Pass REPS=3 (or more) for any run
# whose numbers will be COMPARED rather than merely reported.
# Median, not mean: one slow outlier must not drag a cell, and on this
# box outliers run 35% low when they come.
REPS=${REPS:-1}

# median of whitespace-separated numbers, INTEGER OR DECIMAL - the rates
# are integers and the latencies are ms with three decimals, and an
# integer-only filter here silently turned every p50 into 0.
med() { tr ' ' '\n' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -g | awk '
	{v[NR]=$1} END {
		if (NR == 0) { print 0; exit }
		print (NR % 2) ? v[(NR+1)/2] : (v[NR/2] + v[NR/2+1]) / 2
	}'; }

# GET-only variants, for the COLD-ENTRY arms.  Reads only, and that is
# not a shortcut: a SET through a cold node in PROXY mode would place
# the key THERE, warming the very node whose coldness is the thing being
# measured.
point_get() { # point_get <host> <port> <clients> <pipeline>
	{ cl_bench "$RB_CLI" -h "$1" -p "$2" -t get -n "$(reqs "$4")" -c "$3" -P "$4" \
		-r "$KEYSPACE" -d "$VAL" --csv 2>"$D/rb.err"; echo $? > "$D/rb.rc"; } \
	| tee "$D/rb.out" \
	| awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) == "GET" { gr = unq($2); g5 = unq($5); g9 = unq($7) }
		END { if (gr == "") exit 1; printf "%.0f %s %s\n", gr, g5, g9 }'
}

row_get() { # row_get <arm> <host> <port> <clients> <pipeline>
	ga=$1; gh=$2; gpt=$3; gc=$4; gpl=$5
	gr_=""; g5_=""; g9_=""; gk=1
	while [ $gk -le "$REPS" ]; do
		rg=$(point_get "$gh" "$gpt" "$gc" "$gpl")
		rb_log "$ga" "$gc" "$gpl" "$gk"
		if [ "$(printf '%s' "$rg" | wc -w)" -eq 3 ]; then
			set -- $rg
			gr_="$gr_ $1"; g5_="$g5_ $2"; g9_="$g9_ $3"
		else
			# a failed rep used to be silent here: the median quietly
			# came from fewer reps and nothing said so
			echo "RESPBENCH: $ga c=$gc P=$gpl rep $gk produced no" \
				"result - skipped" >&2
			rb_why
		fi
		gk=$((gk + 1))
	done
	[ -n "$gr_" ] || { echo "RESPBENCH: $ga c=$gc P=$gpl no result" >&2
		return 0; }
	# SET columns are empty: this arm does not write
	printf '%s\t%s\t%s\t\t%s\t\t%s\t\t%s\n' "$ga" "$gc" "$gpl" \
		"$(echo $gr_ | med)" "$(echo $g5_ | med)" "$(echo $g9_ | med)" \
		>> "$D/results.tsv"
	mg=$(echo $gr_ | med)
	trow "$ga" "$gc" "$gpl" "" "$mg" "" "$(echo $g9_ | med)"
}

row() { # row <arm> <host> <port> <clients> <pipeline>
	r_arm=$1; r_host=$2; r_port=$3; r_cli=$4; r_pipe=$5
	r_sets=""; r_gets=""; r_sp50=""; r_gp50=""; r_sp99=""; r_gp99=""
	r_rep=1
	while [ $r_rep -le "$REPS" ]; do
		if routed_ok "$r_cli"; then
			r=$(point_routed "$r_cli" "$r_pipe")
		else
			r=$(point "$r_host" "$r_port" "$r_cli" "$r_pipe")
		fi
		rb_log "$r_arm" "$r_cli" "$r_pipe" "$r_rep"
		if [ "$(printf '%s' "$r" | wc -w)" -ne 6 ]; then
			echo "RESPBENCH: $r_arm c=$r_cli P=$r_pipe rep" \
				"$r_rep produced no result - skipped" >&2
			rb_why
		else
			set -- $r
			r_sets="$r_sets $1"; r_gets="$r_gets $2"
			r_sp50="$r_sp50 $3"; r_gp50="$r_gp50 $4"
			r_sp99="$r_sp99 $5"; r_gp99="$r_gp99 $6"
		fi
		r_rep=$((r_rep + 1))
	done
	# every rep failed: the cell must not reach the table at all
	if [ -z "$r_sets" ]; then
		echo "RESPBENCH: $r_arm c=$r_cli P=$r_pipe produced no" \
			"result in $REPS rep(s) - skipped" >&2
		return 0
	fi
	set -- "$r_arm" "$r_cli" "$r_pipe" \
		"$(echo $r_sets | med)" "$(echo $r_gets | med)" \
		"$(echo $r_sp50 | med)" "$(echo $r_gp50 | med)" \
		"$(echo $r_sp99 | med)" "$(echo $r_gp99 | med)"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" >> "$D/results.tsv"
	[ "$1" = redis ] && ref_put "$2" "$3" "$4" "$5"
	trow "$1" "$2" "$3" "$4" "$5" "$8" "$9"
}

printf '# build=%s date=%s host=%s cpus=%s workers=%s keys=%s reqs=%sxP nmax=%s val=%s reps=%s shard_client=%s\n' \
	"$BINREV" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$NCPU" \
	"$WORKERS" "$KEYSPACE" "$N" "$N_MAX" "$VAL" "$REPS" \
	"$([ "$SHARD_CLUSTER" = 1 ] && echo routed || echo unrouted)" > "$D/results.tsv"
printf 'arm\tclients\tpipeline\tset_rps\tget_rps\tset_p50ms\tget_p50ms\tset_p99ms\tget_p99ms\n' \
	>> "$D/results.tsv"

# ---- arm 1: real redis ------------------------------------------------
echo "=== redis 8, containerised (reference) ===" >&2
start_redis || exit 1
for c in $CLIENTS; do row redis "$REDIS_IP" $REDIS_PORT "$c" 1; done
for p in $PIPES; do
	[ "$p" = 1 ] || row redis "$REDIS_IP" $REDIS_PORT "$PIPE_CLIENTS" "$p"
done
stop_redis

# ---- arm 2: one perfcached -------------------------------------------
# TWO single-node arms, because they are not the same daemon.
#
#   perf1    NO [cluster] section at all - the true like-for-like
#            against redis, one process serving one keyspace.
#   perf1c   a ONE-NODE CLUSTER: same data path, but the cluster plane
#            is running - multicast heartbeats, membership ticks, peer
#            bookkeeping - with nobody to talk to.
#
# The gap between them is the cluster plane's BASELINE cost, and it was
# previously folded into "perf1" and invisible: the harness built one
# config for both arms, so the single-node number already carried it.
echo "=== perfcached, single node, standalone ===" >&2
start_perf 1 none
for c in $CLIENTS; do row perf1 "$(addr 1)" "$(rport 1)" "$c" 1; done
for p in $PIPES; do [ "$p" = 1 ] || row perf1 "$(addr 1)" "$(rport 1)" "$PIPE_CLIENTS" "$p"; done
stop_perf

echo "=== perfcached, single node, one-node cluster ===" >&2
start_perf 1
for c in $CLIENTS; do row perf1c "$(addr 1)" "$(rport 1)" "$c" 1; done
for p in $PIPES; do [ "$p" = 1 ] || row perf1c "$(addr 1)" "$(rport 1)" "$PIPE_CLIENTS" "$p"; done
stop_perf

# ---- arms 3-6: three perfcached, ONE ARM PER MODE --------------------
#
# All four driven through NODE 1's RESP door, because that is the honest
# shape of the migration this project exists for: a Redis client CANNOT
# route: it has no map, no owner hash, and no way to learn one.  So on
# proxy and shard every key that does not belong to node 1 is a FORWARD,
# and that forward is the number a migrating deployment actually pays.
# Comparing the four is therefore comparing what a dumb Redis client
# costs you per mode - not what a cluster-aware libperfd client would.
#
#   perf3-store   pull-on-miss.  A SET lands locally, pushes nowhere.
#   perf3-eager   store + background sweep pushing every record to all
#                 peers.  This is the arm that actually prices
#                 REPLICATION, which store does not.
#   perf3-proxy   one placed copy; non-holder writes forward.
#   perf3-shard   one computable owner; non-owner writes forward.
MODEARMS=${MODEARMS:-"store eager proxy shard"}
for M in $MODEARMS; do
	case $M in
	store)  BLK="mode = store" ;;
	eager)  BLK="mode = eager" ;;
	proxy)  BLK="mode = proxy" ;;
	shard)  BLK="mode = shard" ;;
	*)      echo "RESPBENCH: unknown mode arm '$M'" >&2; continue ;;
	esac
	if [ "$M" = shard ] && [ "$SHARD_CLUSTER" = 1 ]; then
		RB_CLUSTER="--cluster"
		echo "=== perfcached, 3-node cluster, mode=$M (client routes: --cluster) ===" >&2
	else
		RB_CLUSTER=""
		echo "=== perfcached, 3-node cluster, mode=$M ===" >&2
	fi
	start_perf 3 "$BLK"
	[ -n "$RB_CLUSTER" ] && routed_tags
	FLEET_UP=$(date +%s)
	for c in $CLIENTS; do row "perf3-$M" "$(addr 1)" "$(rport 1)" "$c" 1; done
	for p in $PIPES; do
		[ "$p" = 1 ] || row "perf3-$M" "$(addr 1)" "$(rport 1)" \
			"$PIPE_CLIENTS" "$p"
	done

	# The routed arm's GET is a real read now: point_routed's GET phase
	# reads the keys its own SET phase just wrote, on the owner, and the
	# pull counters stay near 0.01% of requests.  Nothing to annotate.
	RB_CLUSTER=""

	# COLD ENTRY.  Driving every mode through node 1 compares a mode at
	# a 100% local hit rate against one at 33%, which measures the
	# PLACEMENT and not the mode.  Measured: proxy read through node 1
	# does 359,712 GET/s and through a node holding nothing 130,208 -
	# landing on shard's 125,313, because proxy and shard relay a
	# non-local key identically and neither caches it at the entry
	# node.  Proxy's lead was the hit rate, nothing else.
	#
	# So: fill through node 1, then read through NODE 2, which holds
	# nothing in proxy and store, and owns only its computed third in
	# shard.  That is the number a client pays when it does not land on
	# the node holding its key - which, with no routing, is most of
	# them.
	# WAIT OUT THE RESHARD GRACE FIRST.  For SHARD_GRACE_S (30s) after a
	# membership change, a shard miss does not answer authoritatively -
	# it retries ONCE as a BROADCAST, because the data may still sit on
	# the old owner (proto.c).  Measured inside that window shard issues
	# 1.15 pulls per request against proxy's 0.79 and looks SLOWER than
	# proxy; measured after it, shard is 2.1x FASTER with an 8.7x
	# tighter p99, which is what its computed-owner unicast should give.
	# Two ad-hoc runs of this comparison disagreed for exactly this
	# reason before the wait existed.  Do not remove it because "the
	# earlier arms take long enough" - they do only for a full CLIENTS
	# list, and a short invocation lands straight inside the window.
	up=$(( $(date +%s) - FLEET_UP ))
	if [ "$up" -lt 32 ]; then
		echo "  (waiting $((32 - up))s for the reshard grace to expire)" >&2
		sleep $((32 - up))
	fi
	cl_bench "$RB_CLI" -h "$(addr 1)" -p "$(rport 1)" -t set -n "$N" \
		-c 20 -r "$KEYSPACE" -d "$VAL" --csv >/dev/null 2>&1
	# The cold arm is deliberately left UNROUTED, shard included: it exists
	# to measure a read entering through a node that holds none of the
	# data, and a cluster-aware client has no such case by construction -
	# it routes to the owner, so "drive node 2" would silently become a
	# second copy of the hot arm rather than a cold one.
	RB_CLUSTER=""
	for c in $CLIENTS; do
		row_get "perf3-$M-cold" "$(addr 2)" "$(rport 2)" "$c" 1
	done
	for p in $PIPES; do
		[ "$p" = 1 ] || row_get "perf3-$M-cold" "$(addr 2)" \
			"$(rport 2)" "$PIPE_CLIENTS" "$p"
	done
	# the eager arm is the only one whose work continues AFTER the
	# drive returns, so it is the only one that must be let finish
	# before the fleet is torn down - otherwise the next arm starts
	# while this one is still sweeping and pays for it.
	if [ "$M" = eager ]; then
		t=0
		while [ $t -lt 60 ]; do
			a=$(statj 1 | jget 'd["collections"][0]["entries"]')
			b=$(statj 2 | jget 'd["collections"][0]["entries"]')
			c3=$(statj 3 | jget 'd["collections"][0]["entries"]')
			a=${a:-0}; b=${b:-0}; c3=${c3:-0}
			[ "$b" -ge "$a" ] && [ "$c3" -ge "$a" ] && break
			sleep 2; t=$((t + 2))
		done
		echo "  eager settled after ${t}s: $a/$b/$c3 entries" >&2
	fi
	stop_perf
done

# ---- the data actually crossed the cluster ---------------------------
#
# This section tests PULL-ON-MISS and therefore needs a STORE fleet
# specifically - it is not mode-agnostic.  It used to inherit whatever
# was left running, which was store only because store was the only
# cluster arm; with four arms the last one is shard, where a read from
# node 2 for a key node 2 OWNS is local and proves nothing about pulls.
# So it starts its own fleet rather than borrowing one.
echo "=== cross-node + failover (store fleet) ===" >&2
start_perf 3 "mode = store"
i=1
while [ $i -le 50 ]; do
	cl_exec "$RB_CLI" redis-cli -h "$(addr 1)" -p "$(rport 1)" SET "xn$i" "v$i" >/dev/null 2>&1
	i=$((i + 1))
done
# NOT waiting for replication - store mode pushes nothing.  The reads
# below MISS on nodes 2 and 3 and pull from node 1 on demand, which is
# the mechanism actually under test here.  The pause only lets the fleet
# settle enough for a pull to find a peer.
sleep 3
XN=""
for n in 2 3; do
	hit=0; i=1
	while [ $i -le 50 ]; do
		v=$(cl_exec "$RB_CLI" redis-cli -h "$(addr $n)" -p "$(rport $n)" GET "xn$i" 2>/dev/null)
		[ "$v" = "v$i" ] && hit=$((hit + 1))
		i=$((i + 1))
	done
	XN="$XN node$n:$hit/50"
done

# per-node view before the kill
for n in 1 2 3; do
	e=$(statj $n | jget 'd["collections"][0]["entries"]')
	RC=$(statj $n | jget 'd["resp"]["conns"]')
	RJ=$(statj $n | jget 'd["resp"]["rejected"]')
	echo "  node$n entries=$e resp_conns=$RC resp_rejected=$RJ" >&2
done

VICTIM=$(nodec 1)
$RT kill -s KILL "$VICTIM" >/dev/null 2>&1; P1=
# Poll for the fleet to notice rather than guessing: PEER_PURGE_MS is
# 6000 and the fixed `sleep 6` this replaces sampled exactly on that
# boundary, which is enough to make a degraded result flip between runs.
DET=""; t=0
while [ $t -lt 30 ]; do
	M=$(cl_exec "$RB_TOOL" perfcli -h "$(addr 2)" -p "$(nport 2)" -a "$SECRET" \
		-j '{"method":"members"}' 2>/dev/null | jget 'len(d["members"])')
	[ -n "$M" ] && [ "$M" -le 2 ] && { DET=$t; break; }
	sleep 1; t=$((t + 1))
done
[ -n "$DET" ] || echo "RESPBENCH: node 2 still lists ${M:-?} members \
after ${t}s - the failover line is suspect" >&2
if [ -z "$VICTIM" ] ||
   $RT inspect -f '{{.State.Running}}' "$VICTIM" 2>/dev/null | grep -q true; then
	echo "RESPBENCH: victim did not die - failover line is meaningless" >&2
	DEAD=no
else
	DEAD=yes
fi
hit=0; i=1
while [ $i -le 50 ]; do
	v=$(cl_exec "$RB_CLI" redis-cli -h "$(addr 2)" -p "$(rport 2)" GET "xn$i" 2>/dev/null)
	[ "$v" = "v$i" ] && hit=$((hit + 1))
	i=$((i + 1))
done
# and still takes writes through the survivor
cl_exec "$RB_CLI" redis-cli -h "$(addr 2)" -p "$(rport 2)" SET afterkill yes >/dev/null 2>&1
AW=$(cl_exec "$RB_CLI" redis-cli -h "$(addr 2)" -p "$(rport 2)" GET afterkill 2>/dev/null)
stop_perf

{
	echo
	echo "cross-node reads (SET via node1's RESP port):$XN"
	echo "after killing node1 (victim_dead=$DEAD, fleet noticed in \
${DET:-never}s): node2 served $hit/50, new write readback=${AW:-FAILED}"
} >> "$D/results.tsv"

echo
echo "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"

# and the comparison table, which is what anyone actually reads: every
# arm against the redis reference at the same cell, with ratios.  Kept
# in its own script so a results file can be re-summarised later without
# re-running anything.
SUMDIR=$(dirname "$0")
[ -x "$SUMDIR/summarize.sh" ] && sh "$SUMDIR/summarize.sh" "$D/results.tsv"
