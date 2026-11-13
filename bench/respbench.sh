#!/bin/sh
# respbench.sh - redis-benchmark through the RESP door against redis, one perfcached (standalone and
# one-node cluster) and a 3-node fleet per mode, all in containers.  usage: bench/respbench.sh
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
# one epoll thread per worker: more workers than CPUs is contention, and 4 at most so a
# bigger host still matches the published numbers
NCPU=$(nproc 2>/dev/null || echo 2)
WORKERS=${WORKERS:-$([ "$NCPU" -lt 4 ] && echo "$NCPU" || echo 4)}
KEYSPACE=${KEYSPACE:-20000}
CLIENTS=${CLIENTS:-"1 4 16 50 200 500 1000"}
PIPES=${PIPES:-"1 8 16 64"}

# client count of the pipelined cells: the one value CLIENTS names, if it names exactly one,
# else 50 (the default matrix and the README's figures)
PIPE_CLIENTS=${PIPE_CLIENTS:-$(set -- $CLIENTS; [ $# -eq 1 ] && echo "$1" || echo 50)}

# shard is driven by a cluster-aware client: unrouted, two thirds of the keys forward and the
# cell measures that hop.  SHARD_CLUSTER=0 gives the unrouted arm.
SHARD_CLUSTER=${SHARD_CLUSTER:-1}
RB_CLUSTER=""                      # "--cluster" while a routed arm runs
CLNODES=3                          # the fleet the mode arms build

# a routed cell needs a connection per node; smaller cells stay unrouted and say so
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

# no host-tool checks: redis-benchmark, redis-cli and perfcli all come from the images
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

# this host is also a CI runner: refuse to start while CI work runs, or the cells measure it.
# Match the work, never the runner daemon, whose command line always matches.
BUSY_CHECK=${BUSY_CHECK:-1}
host_busy() {
	hb_n=0
	for hb_p in /proc/[0-9]*; do
		# comm, not cmdline: a shell whose argv mentions redis-benchmark (this one) must not match
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

# a fleet or redis left by an interrupted run would answer in this one's place: remove those
# containers by name, never by pattern (which would match the shell running the sweep)
cl_rm "$RB_CLI" "$RB_TOOL" "$RCON" rb-n1 rb-n2 rb-n3
cl_net_up "$NET" "$SUBNET" || exit 1
cl_image_build "$IMG" "$CONTAINERFILE" || exit 1
cl_client_up "$RB_CLI" "$NET" || exit 1
cl_client_up "$RB_TOOL" "$NET" "$IMG" || exit 1

# stamp the revision read out of the image that will run, not the tree, and refuse an unstamped
# build: an unstamped figure cannot be reproduced
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

# one persistent client container per image, both on $NET so they measure the bench network
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
	# "none" = no [cluster] section: a one-node cluster still runs the cluster plane (heartbeats,
	# membership tick), so the two are measured separately
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
		# no 'plaintext = loopback': it makes a loopback listener plaintext-only and perfcli
		# authenticates; the RESP door is plaintext regardless
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
# a non-loopback RESP listener needs an allow-list (RESP has no handshake);
# scoped to the bench subnet, which only the containers reach
resp_allow = $SUBNET
$CLUSTERBLK
$([ -n "$CLUSTERBLK" ] && echo "advertise = $(addr $i)")
[collection 0]
buckets_log2 = 17
EOF
		# a container's default memlock is below the arena: mlock() would fail and the arena would run
		# swappable, with nothing in the daemon's log
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

# ref.txt holds the redis cells so every later arm's progress line carries its ratio (POSIX sh
# has no associative arrays)
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

# one aligned table per arm; the header reprints whenever the arm changes
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

# redis-benchmark times a run in whole milliseconds: N x pipeline (capped at N_MAX) keeps every
# cell above a second, so its figures are not on a 2% grid
reqs() { r=$((N * $1)); [ "$r" -gt "$N_MAX" ] && r=$N_MAX; echo "$r"; }
# each redis-benchmark call leaves its rc, stderr and stdout in $D and appends to
# redis-benchmark.log; "Could not fetch server CONFIG" is expected (no CONFIG verb) and filtered
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

# routed driver for the shard arm: redis-benchmark's cluster mode randomises the hash tag, so its
# GETs miss.  Instead one instance per node, on a tag that node owns; rates summed, worst p99.
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

# REPS: passes per cell, median taken.  One pass is enough for the redis/perf1 headline, not to
# compare arms (run-to-run spread ~11% against 14-20% between modes): use REPS=3 for that.
REPS=${REPS:-1}

# median of integers OR decimals: the latencies are ms with three decimals
med() { tr ' ' '\n' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -g | awk '
	{v[NR]=$1} END {
		if (NR == 0) { print 0; exit }
		print (NR % 2) ? v[(NR+1)/2] : (v[NR/2] + v[NR/2+1]) / 2
	}'; }

# GET-only, for the cold-entry arms: a SET through a cold proxy node would place the key there
# and warm the node being measured
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
			# say so: otherwise the median silently comes from fewer reps
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

# ---- arm 2: one perfcached, twice: perf1 has no [cluster] section, perf1c is a one-node
# cluster; the gap is the cluster plane's baseline cost
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

# ---- arms 3-6: three perfcached, one arm per mode, all through node 1's RESP door: a Redis
# client cannot route, so on proxy and shard every key node 1 does not own is a forward
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

	RB_CLUSTER=""

	# cold entry: read through node 2, which holds nothing (or a third in shard), so the mode is
	# measured and not node 1's hit rate.  Wait out SHARD_GRACE_S first: inside it shard misses broadcast.
	up=$(( $(date +%s) - FLEET_UP ))
	if [ "$up" -lt 32 ]; then
		echo "  (waiting $((32 - up))s for the reshard grace to expire)" >&2
		sleep $((32 - up))
	fi
	cl_bench "$RB_CLI" -h "$(addr 1)" -p "$(rport 1)" -t set -n "$N" \
		-c 20 -r "$KEYSPACE" -d "$VAL" --csv >/dev/null 2>&1
	# the cold arm stays unrouted, shard included: a routing client never enters through a node
	# without the data, so routed it would be a copy of the hot arm
	RB_CLUSTER=""
	for c in $CLIENTS; do
		row_get "perf3-$M-cold" "$(addr 2)" "$(rport 2)" "$c" 1
	done
	for p in $PIPES; do
		[ "$p" = 1 ] || row_get "perf3-$M-cold" "$(addr 2)" \
			"$(rport 2)" "$PIPE_CLIENTS" "$p"
	done
	# eager keeps sweeping after the drive returns: let it converge before teardown, or the next arm
	# pays for it
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

# ---- the data crossed the cluster: pull-on-miss needs its own store fleet (on shard a read
# from node 2 of a key it owns would be local)
echo "=== cross-node + failover (store fleet) ===" >&2
start_perf 3 "mode = store"
i=1
while [ $i -le 50 ]; do
	cl_exec "$RB_CLI" redis-cli -h "$(addr 1)" -p "$(rport 1)" SET "xn$i" "v$i" >/dev/null 2>&1
	i=$((i + 1))
done
# store pushes nothing: the reads below miss on nodes 2 and 3 and pull from node 1; the pause
# only lets the fleet settle
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
# poll for the fleet to notice rather than sleeping: a fixed sleep at PEER_PURGE_MS (6 s) flips
# the result between runs
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

# the comparison table, in its own script so a results file can be re-summarised later
SUMDIR=$(dirname "$0")
[ -x "$SUMDIR/summarize.sh" ] && sh "$SUMDIR/summarize.sh" "$D/results.tsv"
