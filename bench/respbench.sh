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
# usage: bench/respbench.sh [./perfcached] [./perfcli]
set -u

BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=${RESPBENCH_DIR:-/var/tmp/respbench}
N=${N:-100000}
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
SECRET=rb-client-secret
P1= P2= P3= PR=

command -v redis-benchmark >/dev/null 2>&1 || {
	echo "respbench: redis-benchmark not installed"; exit 1; }
command -v redis-cli >/dev/null 2>&1 || {
	echo "respbench: redis-cli not installed"; exit 1; }

rm -rf "$D"; mkdir -p "$D"

# PROVENANCE.  The numbers are only worth as much as the build that
# produced them, so stamp the BINARY's revision (not the tree's - they
# differ the moment you run an older binary against a newer checkout)
# and refuse to run one that cannot name itself.  An unstamped build is
# how a page ends up quoting figures nobody can reproduce.
BINREV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "RESPBENCH: refusing to measure an unstamped build ($BINREV) - \
build from a git checkout so PC_BUILD_REV is set" >&2
	exit 1
fi

trap 'for v in "$P1" "$P2" "$P3" "$PR"; do [ -n "$v" ] && kill -9 $v \
     2>/dev/null; done' EXIT TERM INT

# 1000 concurrent clients needs the descriptors for them on BOTH sides;
# the default 1024 is close enough to the ask to trip it
ulimit -n 8192 2>/dev/null || true

# PORT PRE-FLIGHT.  perfcached listeners use SO_REUSEPORT, so a fleet left
# by an interrupted run binds ALONGSIDE this one and the kernel splits the
# traffic between two unrelated fleets.  redis-server is worse: it refuses
# the port, start_redis then PINGs the daemon that was already there, gets
# a PONG, and the whole reference arm measures somebody else's instance.
ports_free() { # ports_free <port>...
	for pf in "$@"; do
		if ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]"; then
			echo "RESPBENCH: port $pf is already bound - this run would \
measure something it did not start:" >&2
			ss -ltnp 2>/dev/null | grep ":$pf[[:space:]]" >&2
			exit 1
		fi
	done
}
ports_free 16499 16401 16402 16403 17401 17402 17403

addr() { echo "127.0.42.$1"; }
nport() { echo "$((17400 + $1))"; }       # native listener
rport() { echo "$((16400 + $1))"; }       # RESP door
REDIS_PORT=16499

statj() { "$CLI" -h "$(addr $1)" -p "$(nport $1)" -a "$SECRET" \
	-j '{"method":"stats"}' 2>/dev/null; }
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
$CLUSTERBLK
$([ -n "$CLUSTERBLK" ] && echo "advertise = $(addr $i)")
[collection 0]
buckets_log2 = 17
EOF
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
		eval "P$i=\$!"
		i=$((i + 1))
	done
	i=1
	while [ $i -le $n ]; do
		k=0
		while [ $k -lt 120 ]; do
			grep -q "perfcached ready" "$D/n$i.log" 2>/dev/null && break
			sleep 0.1; k=$((k + 1))
		done
		i=$((i + 1))
	done
	[ "$n" -gt 1 ] && sleep 4        # membership + election
	return 0
}
stop_perf() {
	for v in "$P1" "$P2" "$P3"; do
		[ -n "$v" ] && kill -9 $v 2>/dev/null
	done
	wait 2>/dev/null
	P1= P2= P3=
	sleep 0.5
}
start_redis() {
	redis-server --port $REDIS_PORT --bind 127.0.0.1 --save '' \
		--appendonly no --protected-mode no --daemonize no \
		> "$D/redis.log" 2>&1 &
	PR=$!
	k=0
	while [ $k -lt 100 ]; do
		redis-cli -p $REDIS_PORT PING 2>/dev/null | grep -q PONG && return 0
		sleep 0.1; k=$((k + 1))
	done
	echo "respbench: redis-server did not come up" >&2
	return 1
}
stop_redis() { [ -n "$PR" ] && kill -9 $PR 2>/dev/null; PR=; sleep 0.3; }

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

point() { # point <host> <port> <clients> <pipeline>
	redis-benchmark -h "$1" -p "$2" -t set,get -n "$N" -c "$3" -P "$4" \
		-r "$KEYSPACE" -d "$VAL" --csv 2>/dev/null \
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
	redis-benchmark -h "$1" -p "$2" -t get -n "$N" -c "$3" -P "$4" \
		-r "$KEYSPACE" -d "$VAL" --csv 2>/dev/null \
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
		if [ "$(printf '%s' "$rg" | wc -w)" -eq 3 ]; then
			set -- $rg
			gr_="$gr_ $1"; g5_="$g5_ $2"; g9_="$g9_ $3"
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
		r=$(point "$r_host" "$r_port" "$r_cli" "$r_pipe")
		if [ "$(printf '%s' "$r" | wc -w)" -ne 6 ]; then
			echo "RESPBENCH: $r_arm c=$r_cli P=$r_pipe rep" \
				"$r_rep produced no result - skipped" >&2
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

printf '# build=%s date=%s host=%s cpus=%s workers=%s keys=%s val=%s reps=%s\n' \
	"$BINREV" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$NCPU" \
	"$WORKERS" "$N" "$VAL" "$REPS" > "$D/results.tsv"
printf 'arm\tclients\tpipeline\tset_rps\tget_rps\tset_p50ms\tget_p50ms\tset_p99ms\tget_p99ms\n' \
	>> "$D/results.tsv"

# ---- arm 1: real redis ------------------------------------------------
echo "=== redis-server (reference) ===" >&2
start_redis || exit 1
for c in $CLIENTS; do row redis 127.0.0.1 $REDIS_PORT "$c" 1; done
for p in $PIPES; do
	[ "$p" = 1 ] || row redis 127.0.0.1 $REDIS_PORT "$PIPE_CLIENTS" "$p"
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
	eager)  BLK="mode = store
eager = 1" ;;
	proxy)  BLK="mode = proxy" ;;
	shard)  BLK="mode = shard" ;;
	*)      echo "RESPBENCH: unknown mode arm '$M'" >&2; continue ;;
	esac
	echo "=== perfcached, 3-node cluster, mode=$M ===" >&2
	start_perf 3 "$BLK"
	FLEET_UP=$(date +%s)
	for c in $CLIENTS; do row "perf3-$M" "$(addr 1)" "$(rport 1)" "$c" 1; done
	for p in $PIPES; do
		[ "$p" = 1 ] || row "perf3-$M" "$(addr 1)" "$(rport 1)" \
			"$PIPE_CLIENTS" "$p"
	done

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
	redis-benchmark -h "$(addr 1)" -p "$(rport 1)" -t set -n "$N" \
		-c 20 -r "$KEYSPACE" -d "$VAL" --csv >/dev/null 2>&1
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
	redis-cli -h "$(addr 1)" -p "$(rport 1)" SET "xn$i" "v$i" >/dev/null 2>&1
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
		v=$(redis-cli -h "$(addr $n)" -p "$(rport $n)" GET "xn$i" 2>/dev/null)
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

VICTIM=$P1
kill -9 $P1 2>/dev/null; P1=
# Poll for the fleet to notice rather than guessing: PEER_PURGE_MS is
# 6000 and the fixed `sleep 6` this replaces sampled exactly on that
# boundary, which is enough to make a degraded result flip between runs.
DET=""; t=0
while [ $t -lt 30 ]; do
	M=$("$CLI" -h "$(addr 2)" -p "$(nport 2)" -a "$SECRET" \
		-j '{"method":"members"}' 2>/dev/null | jget 'len(d["members"])')
	[ -n "$M" ] && [ "$M" -le 2 ] && { DET=$t; break; }
	sleep 1; t=$((t + 1))
done
[ -n "$DET" ] || echo "RESPBENCH: node 2 still lists ${M:-?} members \
after ${t}s - the failover line is suspect" >&2
if [ -z "$VICTIM" ] || kill -0 "$VICTIM" 2>/dev/null; then
	echo "RESPBENCH: victim did not die - failover line is meaningless" >&2
	DEAD=no
else
	DEAD=yes
fi
hit=0; i=1
while [ $i -le 50 ]; do
	v=$(redis-cli -h "$(addr 2)" -p "$(rport 2)" GET "xn$i" 2>/dev/null)
	[ "$v" = "v$i" ] && hit=$((hit + 1))
	i=$((i + 1))
done
# and still takes writes through the survivor
redis-cli -h "$(addr 2)" -p "$(rport 2)" SET afterkill yes >/dev/null 2>&1
AW=$(redis-cli -h "$(addr 2)" -p "$(rport 2)" GET afterkill 2>/dev/null)
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
