#!/bin/sh
# containerbench.sh - Redis vs perfcached over RESP and the native door, built and run in containers
# (needs only a runtime).  Rows cross a bridge veth: compare arms here, not with respbench's host rows.
set -u

# pick a runtime that can actually BUILD (nerdctl answers `info`, then fails every build without
# buildkitd), probed with a real one-line image
RT=${RUNTIME:-}
# the probe runs a real RUN step on the real build's base image: a probe without one passed on a
# host where AppArmor blocked every RUN step
PROBE_ERR=""
BUILD_ENV=""

# do_build <runtime> <-f file> <args...> - shared by the probe and the real build.  Docker tries
# BuildKit first: legacy RUN steps die (exit 185) where the docker-default AppArmor profile won't load.
do_build() {
	dbrt=$1; shift
	if [ "$dbrt" = docker ]; then
		out=$(DOCKER_BUILDKIT=1 docker build "$@" 2>&1) && {
			BUILD_ENV="DOCKER_BUILDKIT=1"; printf '%s' "$out"; return 0; }
		# BuildKit unavailable or refused - try the legacy builder
		out2=$(DOCKER_BUILDKIT=0 docker build "$@" 2>&1) && {
			BUILD_ENV="DOCKER_BUILDKIT=0"; printf '%s' "$out2"; return 0; }
		printf 'buildkit attempt:\n%s\nlegacy attempt:\n%s' "$out" "$out2"
		return 1
	fi
	out=$("$dbrt" build "$@" 2>&1) && { printf '%s' "$out"; return 0; }
	printf '%s' "$out"; return 1
}

probe_build() {
	pd=$(mktemp -d) || return 1
	printf 'FROM docker.io/library/debian:13-slim\nRUN true\n' \
		> "$pd/Containerfile"
	PROBE_ERR=$(do_build "$1" -f "$pd/Containerfile" -t cb-probe:x "$pd")
	rc=$?
	"$1" rmi cb-probe:x >/dev/null 2>&1
	rm -rf "$pd"
	return $rc
}

# name the cause when the runtime's own message is unhelpful
explain_build_failure() {
	case "$1" in
	*MAP_HUGETLB*|*hugetlb*|*huge*)
		echo "  -> the node exited just after taking huge pages.  On a"
		echo "     host with a RESERVED pool (nr_hugepages) the arenas"
		echo "     come out of that pool, and three nodes at ARENA=$ARENA MB"
		echo "     need $((ARENA * 3)) MB of it.  Check:"
		echo "       grep Huge /proc/meminfo"
		echo "     Then either lower it - ARENA=64 $0 - or raise the"
		echo "     pool.  A killed node does not return its pages"
		echo "     instantly, so back-to-back arms can fail for this"
		echo "     reason even when one arm alone succeeds." ;;
	*apparmor*|*AppArmor*)
		echo "  -> AppArmor refused to load the docker-default profile,"
		echo "     so no build container can start."
		if [ "$(systemd-detect-virt --container 2>/dev/null)" = lxc ] ||
		   grep -qa 'container=lxc' /proc/1/environ 2>/dev/null; then
			echo ""
			echo "     THIS IS AN LXC CONTAINER, and that is the cause."
			echo "     A guest cannot write profiles into the host's"
			echo "     AppArmor namespace, so nested Docker cannot start"
			echo "     containers at all - nothing inside the guest can"
			echo "     fix it.  On the LXC HOST:"
			echo "       LXD:      lxc config set <name> security.nesting true"
			echo "       Proxmox:  Options -> Features -> Nesting"
			echo "       lxc.conf: lxc.apparmor.profile = unconfined"
			echo "     then restart the guest."
			echo ""
			echo "     Or use podman inside the guest, which does not"
			echo "     load an AppArmor profile: apt install podman"
		else
			echo "     Fixes, cheapest first:"
			echo "       systemctl restart apparmor docker"
			echo "       apt install --reinstall apparmor   (version skew)"
			echo "     This affects ALL docker builds on this host, not"
			echo "     just this script - worth fixing regardless."
		fi ;;
	*buildkit*|*buildkitd*)
		echo "  -> nerdctl needs buildkitd: systemctl start buildkit" ;;
	*"Cannot connect"*|*"daemon"*)
		echo "  -> the daemon is not reachable: systemctl start docker" ;;
	esac
}
if [ -n "$RT" ]; then
	command -v "$RT" >/dev/null 2>&1 || {
		echo "containerbench: RUNTIME=$RT is not in PATH"; exit 1; }
	probe_build "$RT" || {
		echo "containerbench: RUNTIME=$RT cannot build images here."
		echo "  it said:"
		printf '%s\n' "$PROBE_ERR" | sed 's/^/    /' | tail -6
		explain_build_failure "$PROBE_ERR"
		exit 1; }
else
	tried=""
	for c in podman nerdctl docker; do
		command -v $c >/dev/null 2>&1 || continue
		if probe_build $c; then RT=$c; break; fi
		# keep WHY, not just THAT - "cannot build" with no reason
		# sends people to install things they already have
		tried="$tried
--- $c said: ---
$(printf '%s' "$PROBE_ERR" | tail -4)
$(explain_build_failure "$PROBE_ERR")"
		echo "containerbench: $c is installed but cannot build - trying next" >&2
	done
	[ -n "$RT" ] || {
		echo "containerbench: no container runtime here can build an image."
		printf '%s\n' "$tried" | sed 's/^/  /'
		echo ""
		echo "  Common causes:"
		echo "    docker   - dockerd not running:  systemctl start docker"
		echo "    nerdctl  - buildkitd not running: systemctl start buildkit"
		echo "    podman   - usually works as-is;   apt install podman"
		echo ""
		echo "  Or point it at a runtime explicitly: RUNTIME=docker $0"
		exit 1; }
fi

D=${CONTAINERBENCH_DIR:-/var/tmp/containerbench}
NET=${NET:-pcbenchnet}
# MTU defaults to the default-route interface's, not the runtime's 1500: read throughput tracks MTU,
# so a 9000-byte host benched at 1500 measures a network it does not have.
host_mtu() {
	hi=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
	[ -n "$hi" ] && ip link show "$hi" 2>/dev/null |
		sed -n 's/.*mtu \([0-9]*\).*/\1/p' | head -1
}
MTU=${MTU:-$(host_mtu)}
case "${MTU:-}" in ''|*[!0-9]*) MTU=1500 ;; esac
SUBNET=${SUBNET:-10.98.0.0/24}
IMG=${IMG:-perfcached:bench}
# Debian (glibc), never a musl tag: cb-drive runs redis-benchmark from this image, so a musl tag
# would drive the resp arms with a musl-built client.
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
CLIENTS=${CLIENTS:-"1 4 16 50 200 500 1000"}
PIPES=${PIPES:-"1 8 16 64"}

# PIPE_CLIENTS: the client count of the pipelined cells, which walk depth at fixed clients: CLIENTS
# when it names exactly one value, else 50.
PIPE_CLIENTS=${PIPE_CLIENTS:-$(set -- $CLIENTS; [ $# -eq 1 ] && echo "$1" || echo 50)}
REPS=${REPS:-3}
N=${N:-100000}
KEYSPACE=${KEYSPACE:-20000}
VAL=${VAL:-200}
# 128, not 512: phase 2 runs three nodes, and on a host with a reserved hugepage pool 3 x 512 MB
# leaves node 2 nothing after its "probed tier = MAP_HUGETLB".
ARENA=${ARENA:-128}
# WORKERS: half the cores, because the client shares this box (at workers = cores GET fell 28% and
# its p99 tripled).  WORKERS=4 reproduces the README figures on a >=8-core host.
NCPU=$(nproc 2>/dev/null || echo 2)
WORKERS=${WORKERS:-$([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)}

# CPUSET_SRV/CPUSET_CLI (+ MEMS_SRV): pin servers and client apart so neither core contention nor a
# far-socket arena is in the number, e.g. CPUSET_SRV=0-3 MEMS_SRV=0 CPUSET_CLI=4-7.
CPUSET_SRV=${CPUSET_SRV:-}
CPUSET_CLI=${CPUSET_CLI:-}
MEMS_SRV=${MEMS_SRV:-}
PIN_SRV=""
[ -n "$CPUSET_SRV" ] && PIN_SRV="--cpuset-cpus=$CPUSET_SRV"
[ -n "$MEMS_SRV" ] && PIN_SRV="$PIN_SRV --cpuset-mems=$MEMS_SRV"
PIN_CLI=""
[ -n "$CPUSET_CLI" ] && PIN_CLI="--cpuset-cpus=$CPUSET_CLI"
PHASES=${PHASES:-"1 2"}
MODEARMS=${MODEARMS:-"store eager proxy shard spread"}
SECRET=cb-client-secret

# DIALECTS: resp = the RESP door via redis-benchmark; bin = the native door via natbench through
# libperfd's perfd_submit_kv.  The redis reference arm is always resp.
DIALECTS=${DIALECTS:-"resp bin"}
NAT_PORT=6479                          # the native listener; RESP is 6380
NAT_SECS=${NAT_SECS:-5}                # natbench runs for a DURATION
# 1 = time every request, as redis-benchmark does; raising it buys natbench throughput, but the
# resp and native rows then no longer pay the same cost
NAT_LAT=${NAT_LAT:-1}
# scale redis-benchmark's request count with depth and clients so every cell runs a comparable wall
# time (natbench runs for a duration); the client factor saturates where throughput does
n_for() { # n_for <pipeline> <clients>
	if [ "$2" -le 4 ]; then nf_f=1
	elif [ "$2" -le 16 ]; then nf_f=3
	else nf_f=4
	fi
	nf=$((N * $1 * nf_f))
	[ "$nf" -gt "$N_MAX" ] && nf=$N_MAX
	echo "$nf"
}
N_MAX=${N_MAX:-4000000}

# threaded redis-benchmark stops on a 250 ms tick, rounding each pass UP to a quarter second, so a
# pilot pass sizes each cell to run >= CELL_MIN_S (one tick <= 2.5%); 0 restores n_for's sizing
CELL_MIN_S=${CELL_MIN_S:-10}           # whole seconds
CELL_TICK_MS=250                       # redis-benchmark's SHOW_THROUGHPUT_INTERVAL
CELL_N_MAX=${CELL_N_MAX:-200000000}
case "$CELL_MIN_S" in ''|*[!0-9]*) echo "CELL_MIN_S must be whole seconds" >&2; exit 2 ;; esac
cell_ms() { # cell_ms <requests> <rps> -> the pass time, in ms, they imply
	awk -v n="$1" -v r="$2" 'BEGIN { printf "%.0f\n", (r > 0) ? n * 1000 / r : 0 }'
}
fastest() { # fastest <rps> [<rps>] - the higher of the two; empty is ignored
	if [ -n "${2:-}" ] && [ "${2%.*}" -gt "${1%.*}" ]; then echo "$2"; else echo "$1"; fi
}
cell_resize() { # cell_resize <requests> <rps> -> a count for a CELL_MIN_S pass,
	# or nothing when <requests> already ran that long
	awk -v n="$1" -v r="$2" -v min="$CELL_MIN_S" -v tick="$CELL_TICK_MS" \
		-v cap="$CELL_N_MAX" 'BEGIN {
		if (min <= 0 || r <= 0) exit
		ms = n * 1000 / r
		if (ms >= min * 1000) exit
		# it finished somewhere inside the tick before the one that
		# stopped it: size from the fastest rate that allows
		d = ms - tick; if (d < tick) d = tick
		nn = n * 1000 / d * min * 1.1
		if (nn > cap) nn = cap
		printf "%.0f\n", nn
	}'
}
cell_floor() { # cell_floor <arm> <clients> <pipeline> <rep> <requests> <rps>
	[ "$CELL_MIN_S" -gt 0 ] || return 0
	cf_ms=$(cell_ms "$5" "$6")
	[ "$cf_ms" -gt 0 ] && [ "$cf_ms" -lt $((CELL_MIN_S * 1000)) ] || return 0
	say "  $1 c=$2 P=$3 rep $4: ran $cf_ms ms, under the ${CELL_MIN_S}s floor -" \
		"one 250 ms tick is $((25000 / cf_ms))% of this figure"
}
cell_rec() { # cell_rec <arm> <clients> <pipeline> <requests> <set rps|""> <get rps>
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" \
		"$([ -z "$5" ] || cell_ms "$4" "$5")" "$(cell_ms "$4" "$6")" \
		>> "$D/cells.tsv"
}

# glibc only: musl's allocator serialises libperfd's per-reply malloc, so a musl client peaked at
# two threads and the native figures measured the allocator
CONTAINERFILE=${CONTAINERFILE:-bench/Containerfile.debian}
# extra lines for every node's [cluster] block, e.g. max_pending = 64 to provoke a refusal and test
# the error-capture path on a real failure
CLUSTER_EXTRA=${CLUSTER_EXTRA:-}

# ROUTE=1: the native arms route per key, as a cluster-aware deployment does; redis-benchmark cannot,
# so compare bin rows across modes and redis with resp, never resp with native.
ROUTE=${ROUTE:-1}

# list knobs are whitespace-separated; a comma is accepted, then every value is validated so a typo
# cannot measure the default arm under the wrong label
list_norm() { printf '%s' "$1" | tr ',' ' '; }
CLIENTS=$(list_norm "$CLIENTS")
PIPES=$(list_norm "$PIPES")
PHASES=$(list_norm "$PHASES")
MODEARMS=$(list_norm "$MODEARMS")
DIALECTS=$(list_norm "$DIALECTS")

for dl_ in $DIALECTS; do
	case $dl_ in
	resp|bin) ;;
	json) echo "containerbench: the json dialect is gone (S317: the" >&2
	      echo "  native door speaks the binary protocol only) - use bin" >&2
	      exit 2 ;;
	*) echo "containerbench: unknown dialect '$dl_'" >&2
	   echo "  DIALECTS must be some of: resp bin" >&2
	   exit 2 ;;
	esac
done
for m_ in $MODEARMS; do
	case $m_ in
	store|eager|proxy|shard|spread) ;;
	*) echo "containerbench: unknown mode '$m_'" >&2
	   echo "  MODEARMS must be some of: store eager proxy shard spread" >&2
	   exit 2 ;;
	esac
done
for p_ in $PHASES; do
	case $p_ in
	1|2) ;;
	*) echo "containerbench: unknown phase '$p_' (want 1 and/or 2)" >&2
	   exit 2 ;;
	esac
done
# client threads: the same budget for both drivers (redis-benchmark is single-threaded unless told),
# capped at the client count
CLI_THREADS=${CLI_THREADS:-$([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)}
NAT_THREADS=${NAT_THREADS:-}

# 10.98.0.10 redis | .11 standalone perfcached | .21-.23 the cluster
R_IP=10.98.0.10;  R_PORT=6379
S_IP=10.98.0.11
n_ip() { echo "10.98.0.2$1"; }
RESP_PORT=6380

say() { echo "$@" >&2; }
have_phase() { case " $PHASES " in *" $1 "*) return 0;; esac; return 1; }

# NET_KEEP=1: use an EXISTING network and do not remove it at exit
NET_KEEP=${NET_KEEP:-0}

# NETEM: a root qdisc on each container's host-side veth (the bridge device's shapes nothing), so it
# shapes traffic INTO that container: delay and rate are per direction, the RTT twice the delay.
NETEM=${NETEM:-}

host_veth() { # host_veth <container> -> the veth on THIS side of it
	hv_pid=$($RT inspect -f '{{.State.Pid}}' "$1" 2>/dev/null)
	[ -n "$hv_pid" ] || return 1
	# `ip link`, not /sys/class/net/eth0/iflink: sysfs reflects the netns that mounted it, not
	# nsenter's.  The container side prints `eth0@if12`; 12 is the host-side index.
	hv_peer=$(nsenter -t "$hv_pid" -n ip -o link show eth0 2>/dev/null |
		sed -n 's/.*eth0@if\([0-9]*\).*/\1/p' | head -1)
	[ -n "$hv_peer" ] || return 1
	ip -o link 2>/dev/null | awk -F': ' -v p="$hv_peer" \
		'$1 + 0 == p + 0 { split($2, a, "@"); print a[1]; exit }'
}

shape() { # shape <container> - netem on the path INTO this container
	[ -n "$NETEM" ] || return 0
	sh_if=$(host_veth "$1") || {
		say "  NETEM: could not find the veth for $1 - NOT shaped"
		SHAPE_FAIL=1; return 0; }
	# `replace`, not `add`: under NET_KEEP=1 a veth can come back still shaped, and `add` fails EEXIST
	if ! tc qdisc replace dev "$sh_if" root netem $NETEM 2>/dev/null; then
		say "  NETEM: tc failed on $sh_if - NOT shaped (need tc, and"
		say "  CAP_NET_ADMIN on the host)"
		SHAPE_FAIL=1; return 0
	fi
	# read it back - tc exits 0 for a qdisc that did not take
	if tc qdisc show dev "$sh_if" 2>/dev/null | grep -q netem; then
		say "  NETEM into $1 ($sh_if): $NETEM"
	else
		say "  NETEM: no netem on $sh_if after replace - NOT shaped"
		SHAPE_FAIL=1
	fi
}

# the client lives in one persistent container, shaped like the servers, and each measurement is an
# exec into it: an unshaped client gets the whole reply direction free
drive_up() {
	$RT inspect -f '{{.State.Running}}' cb-drive 2>/dev/null | grep -q true || {
		$RT rm -f cb-drive >/dev/null 2>&1
		$RT run -d --name cb-drive --network "$NET" $PIN_CLI \
			--entrypoint tail \
			"$REDIS_IMG" -f /dev/null >/dev/null 2>&1 || {
			say "  could not start the client container cb-drive"
			return 1; }
	}
	# re-shaped even when already up: a container left by a killed run carries that run's qdisc, or none
	shape cb-drive
	pin_check cb-drive "$CPUSET_CLI"
}

bench_cli() { $RT exec cb-drive redis-benchmark "$@"; }

# the native arms need our image (natbench links libperfd), so they get their own client container,
# shaped and pinned by the same calls as cb-drive
drive_nat_up() {
	$RT inspect -f '{{.State.Running}}' cb-driven 2>/dev/null | grep -q true || {
		$RT rm -f cb-driven >/dev/null 2>&1
		$RT run -d --name cb-driven --network "$NET" $PIN_CLI \
			--entrypoint tail \
			"$IMG" -f /dev/null >/dev/null 2>&1 || {
			say "  could not start the native client container cb-driven"
			return 1; }
	}
	shape cb-driven
	pin_check cb-driven "$CPUSET_CLI"
}
nat_cli() { $RT exec cb-driven natbench "$@"; }

cli_threads() { # cli_threads <clients> - never more threads than connections
	ct=$CLI_THREADS
	[ "$ct" -gt "$1" ] && ct=$1
	[ "$ct" -lt 1 ] && ct=1
	echo "$ct"
}
nat_threads() { # nat_threads <conns> - NAT_THREADS overrides, else shared
	nt=${NAT_THREADS:-$CLI_THREADS}
	[ "$nt" -gt "$1" ] && nt=$1
	[ "$nt" -lt 1 ] && nt=1
	echo "$nt"
}

# nat_run <host> <conns> <depth> <getpct> -> "rps p50 p99"; fails on no result, errors or zero hits.
# natbench's 12th positional kv=1 drives the data verbs on their fixed-layout frames (the bin arm).
nat_run() {
	pt_files
	nat_cli "$1" "$NAT_PORT" "$SECRET" 0 "$2" "$(nat_threads "$2")" \
		"$3" "$NAT_SECS" "$KEYSPACE" "$VAL" "$4" 1 "$NAT_LAT" \
		-1 "$ROUTE" \
		>"$PT_OUT" 2>"$PT_ERR"
	awk '
		/ZERO HITS/ { zero = 1 }
		/ops\/s/ {
			ops = $1
			for (i = 1; i <= NF; i++)
				if ($i ~ /^errors=/) { e = $i; sub("errors=", "", e) }
		}
		/^ *p50=/ {
			p50 = $1; p99 = $2
			sub("p50=", "", p50); sub("ms", "", p50)
			sub("p99=", "", p99); sub("ms", "", p99)
		}
		END {
			if (ops == "" || zero || e + 0 > 0) exit 1
			if (p50 == "") { p50 = "-"; p99 = "-" }
			print ops, p50, p99
		}' "$PT_OUT"
}

# two passes per point, getpct 0 then 100, so SET/GET mean what they do for redis-benchmark and the
# GET pass reads the keys the SET pass wrote
point_nat() { # point_nat <host> <clients> <depth>
	pn_s=$(nat_run "$1" "$2" "$3" 0)   || return 1
	pn_g=$(nat_run "$1" "$2" "$3" 100) || return 1
	set -- $pn_s; pn_sr=$1; pn_s5=$2; pn_s9=$3
	set -- $pn_g; pn_gr=$1; pn_g5=$2; pn_g9=$3
	printf '%s %s %s %s %s %s\n' "$pn_sr" "$pn_gr" "$pn_s5" "$pn_g5" \
		"$pn_s9" "$pn_g9"
}

point_nat_get() { # point_nat_get <host> <clients> <depth>
	nat_run "$1" "$2" "$3" 100
}

# read the pinning back: an unverified knob still prints a number
pin_check() { # pin_check <container> <expected-cpus>
	[ -n "$2" ] || return 0
	pc_got=$($RT inspect -f '{{.HostConfig.CpusetCpus}}' "$1" 2>/dev/null)
	if [ "$pc_got" = "$2" ]; then
		say "  pinned $1 to CPUs $pc_got"
	else
		say "  *** $1 IS NOT PINNED: asked for '$2', runtime reports"
		say "  *** '${pc_got:-empty}' - the arms are not comparable"
	fi
}

# prove the shaping bites both ways: with `delay Dms` at each end a PING must come back at ~2D;
# near 1D means the reply path is still free
shape_selftest() { # shape_selftest <host> <port>
	[ -n "$NETEM" ] || return 0
	sd_d=$(printf '%s' "$NETEM" | sed -n 's/.*delay[ ]*\([0-9.]*\)ms.*/\1/p')
	[ -n "$sd_d" ] || return 0
	sd_got=$(bench_cli -h "$1" -p "$2" -t ping -n 200 -c 1 --csv 2>/dev/null |
		awk -F',' 'NR == 2 { gsub(/"/, "", $5); print $5; exit }')
	[ -n "$sd_got" ] || { say "  NETEM selftest: no PING result - NOT verified"
		return 0; }
	say "  NETEM selftest: PING p50 ${sd_got} ms vs $(awk -v d="$sd_d" \
		'BEGIN { printf "%.1f", 2 * d }') ms expected (2 x ${sd_d} ms)"
	awk -v g="$sd_got" -v d="$sd_d" 'BEGIN { exit !(g < 1.5 * d) }' && {
		say "  *** ONE-DIRECTIONAL: the round trip is ~1x the delay, so"
		say "  *** replies are unshaped.  This is NOT a symmetric link and"
		say "  *** the arms are not comparable to one."; }
}
cleanup() {
	$RT rm -f cb-redis cb-solo cb-n1 cb-n2 cb-n3 cb-drive cb-driven \
		>/dev/null 2>&1
	[ "$NET_KEEP" = 1 ] || $RT network rm $NET >/dev/null 2>&1
}
trap 'cleanup' EXIT INT TERM

rm -rf "$D"; mkdir -p "$D"

# ---- assemble -------------------------------------------------------
say "=== building $IMG from $CONTAINERFILE (runtime: $RT) ==="
REV=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
do_build "$RT" --platform linux/amd64 -f "$CONTAINERFILE" \
	--build-arg REV="$REV" -t "$IMG" . > "$D/build.log" 2>&1 || {
	say "containerbench: image build FAILED with $RT - see $D/build.log"
	tail -12 "$D/build.log" >&2
	explain_build_failure "$(cat "$D/build.log")" >&2
	say ""
	say "  Other runtimes on this host, if any, can be tried with"
	say "  RUNTIME=<name> $0"
	exit 1; }

# the image must name the binary inside it: a results file that cannot say what produced it is
# not a measurement
BINREV=$($RT run --rm --entrypoint perfcached "$IMG" -V 2>/dev/null |
	sed -n 's/.*(\(.*\)).*/\1/p')
case "${BINREV:-unknown}" in
unknown|"") say "containerbench: image reports no revision - refusing"; exit 2;;
esac
say "    image built: perfcached $BINREV"

cleanup
# docker wants com.docker.network.driver.mtu and silently ignores `--opt mtu=` (podman, nerdctl),
# which is why the MTU is read back from a container below
case "$RT" in
docker) MTUOPT="com.docker.network.driver.mtu=$MTU" ;;
*)      MTUOPT="mtu=$MTU" ;;
esac
if [ "$NET_KEEP" = 1 ]; then
	$RT network inspect "$NET" >/dev/null 2>&1 || {
		say "containerbench: NET_KEEP=1 but network '$NET' does not"
		say "exist - create it first, then attach your qdisc to its"
		say "bridge, then re-run."; exit 1; }
	say "    using EXISTING network $NET (NET_KEEP=1, not removing it)"
fi
[ "$NET_KEEP" = 1 ] ||
$RT network create --subnet "$SUBNET" --opt "$MTUOPT" "$NET" >/dev/null 2>&1 ||
	$RT network create --subnet "$SUBNET" "$NET" >/dev/null 2>&1 || {
	say "containerbench: could not create network $NET"; exit 1; }
# sysfs, not `ip link`: iproute2 is not in debian-slim
ACT_MTU=$($RT run --rm --network "$NET" --entrypoint sh \
	"$REDIS_IMG" -c 'cat /sys/class/net/eth0/mtu' 2>/dev/null |
	tr -dc '0-9')
say "    network $NET: MTU ${ACT_MTU:-?} (host default route: $(host_mtu))"
if [ -n "$ACT_MTU" ] && [ "$ACT_MTU" = 1500 ] && [ "${MTU}" != 1500 ]; then
	say "    WARNING: asked for MTU $MTU and got 1500 - read figures"
	say "    will understate what this host's network can do."
fi
$RT pull "$REDIS_IMG" >/dev/null 2>&1 || say "    (redis image already present)"
drive_up || exit 1
case " $DIALECTS " in *" bin "*) drive_nat_up || exit 1 ;; esac

printf '# build=%s date=%s host=%s runtime=%s cpus=%s workers=%s arena_mb=%s keys=%s nbase=%s val=%s reps=%s mtu=%s cpuset_srv=%s cpuset_cli=%s mems_srv=%s dialects=%s nat_secs=%s nat_lat=%s cli_threads=%s n_max=%s base=%s redis_img=%s route=%s cell_min_s=%s\n' \
	"$BINREV" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$RT" \
	"$NCPU" "$WORKERS" "$ARENA" "$KEYSPACE" "$N" "$VAL" "$REPS" "${ACT_MTU:-?}" \
	"${CPUSET_SRV:-none}" "${CPUSET_CLI:-none}" "${MEMS_SRV:-none}" \
	"$(echo $DIALECTS | tr ' ' ',')" "$NAT_SECS" "$NAT_LAT" "$CLI_THREADS" "$N_MAX" "$(basename "$CONTAINERFILE" | sed 's/^Containerfile\.//')" \
	"$(basename "$REDIS_IMG")" "$ROUTE" "$CELL_MIN_S" \
	> "$D/results.tsv"
printf 'arm\tclients\tpipeline\tset_rps\tget_rps\tset_p50ms\tget_p50ms\tset_p99ms\tget_p99ms\n' \
	>> "$D/results.tsv"
printf 'arm\tclients\tpipeline\trequests\tset_ms\tget_ms\n' > "$D/cells.tsv"

# ---- the driver -----------------------------------------------------
# ratio against the redis reference, which always runs first; kept in a file (POSIX sh has no arrays)
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

# the client's stderr is kept: it alone tells apart the ways a cluster request is refused
# (TRYAGAIN, forward failed, holder rejected, holder timed out)
PT_OUT=""; PT_ERR=""
pt_files() {                           # $D exists only after setup
	PT_OUT="$D/point.out"; PT_ERR="$D/point.err"
}
point_why() {
	pt_files
	pw=$(grep -hoiE 'Error from server:.*|Could not connect[^\n]*|ZERO HITS' \
		"$PT_ERR" "$PT_OUT" 2>/dev/null | head -1)
	[ -n "$pw" ] || pw=$(grep -hvE '^\s*$' "$PT_ERR" 2>/dev/null | tail -1)
	[ -n "$pw" ] || return 0
	printf ' - %s' "$(printf '%s' "$pw" | cut -c1-100)"
}

point() { # point <host> <port> <clients> <pipeline> [requests]
	pt_files
	case ${DIALECT:-resp} in
	bin)  point_nat "$1" "$3" "$4"; return $? ;;
	resp) [ "${ROUTED_MODE:-0}" = 1 ] &&
	      { point_routed "$3" "$4" "${5:-$(n_for "$4" "$3")}"; return $?; } ;;
	*) say "  BUG: unknown DIALECT '$DIALECT' reached point()"; return 1 ;;
	esac
	bench_cli -h "$1" -p "$2" -t set,get -n "${5:-$(n_for "$4" "$3")}" -c "$3" \
		--threads "$(cli_threads "$3")" \
		-P "$4" -r "$KEYSPACE" -d "$VAL" --csv \
		>"$PT_OUT" 2>"$PT_ERR"
	awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) == "SET" { sr = unq($2); sp50 = unq($5); sp99 = unq($7) }
		unq($1) == "GET" { gr = unq($2); gp50 = unq($5); gp99 = unq($7) }
		END { if (sr == "" || gr == "") exit 1
		      printf "%.0f %.0f %s %s %s %s\n", sr, gr, sp50, gp50, sp99, gp99 }' \
		"$PT_OUT"
}

med() { tr ' ' '\n' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -g | awk '
	{v[NR]=$1} END { if (NR==0) { print 0; exit }
		print (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2 }'; }

# routed RESP: one redis-benchmark per node, each key's hash tag owned by that node per CLUSTER NODES
# (--cluster misses its own keys); rates are summed, p50/p99 the worst of the three.
RTAGS=""; RVAL=""
cb_cli() { $RT run --rm --network "$NET" --entrypoint redis-cli "$REDIS_IMG" \
	-h "$(n_ip 1)" -p "$RESP_PORT" "$@" 2>/dev/null; }
routed_tags() {                    # -> RTAGS, one hash tag per owner
	RTAGS=""; rt_i=1
	while [ $rt_i -le 3 ]; do
		rt_found=""
		for rt_t in t0 t1 t2 t3 t4 t5 t6 t7 t8 t9 ta tb tc td te tf \
		            u0 u1 u2 u3 u4 u5 u6 u7 u8 u9 v0 v1 v2 v3 v4 v5; do
			rt_s=$(cb_cli CLUSTER KEYSLOT "key:{$rt_t}:0")
			[ -n "$rt_s" ] || continue
			rt_o=$(cb_cli CLUSTER NODES |
				awk -v s="$rt_s" '{ip=$2; sub(/:.*/,"",ip);
					for(i=9;i<=NF;i++){split($i,r,"-"); lo=r[1];
					hi=(r[2]==""?r[1]:r[2]);
					if(s+0>=lo+0 && s+0<=hi+0){print ip; exit}}}')
			[ "$rt_o" = "$(n_ip $rt_i)" ] && { rt_found=$rt_t; break; }
		done
		[ -n "$rt_found" ] || {
			say "  no hash tag maps to $(n_ip $rt_i) in CLUSTER NODES -" \
				"the routed arm cannot be measured here"
			return 1; }
		RTAGS="$RTAGS $rt_found"
		rt_i=$((rt_i + 1))
	done
	RVAL=$(printf '%*s' "$VAL" '' | tr ' ' x)
	say "  routed tags:$RTAGS - one hash tag per owner, from CLUSTER NODES"
	return 0
}
point_routed() { # point_routed <clients> <pipeline> <requests>
	pt_files
	pr_c=$(( $1 / 3 )); [ "$pr_c" -lt 1 ] && pr_c=1
	pr_i=1; pr_pids=""; : > "$PT_ERR"
	rm -f "$D"/rr-s* "$D"/rr-g*
	for pr_tag in $RTAGS; do
		(
			bench_cli -h "$(n_ip $pr_i)" -p "$RESP_PORT" -n "$3" -c "$pr_c" \
				--threads "$(cli_threads "$pr_c")" -P "$2" -r "$KEYSPACE" \
				--csv SET "key:{$pr_tag}:__rand_int__" "$RVAL" \
				> "$D/rr-s$pr_i" 2>>"$PT_ERR"
			bench_cli -h "$(n_ip $pr_i)" -p "$RESP_PORT" -n "$3" -c "$pr_c" \
				--threads "$(cli_threads "$pr_c")" -P "$2" -r "$KEYSPACE" \
				--csv GET "key:{$pr_tag}:__rand_int__" \
				> "$D/rr-g$pr_i" 2>>"$PT_ERR"
		) &
		pr_pids="$pr_pids $!"
		pr_i=$((pr_i + 1))
	done
	for pr_p in $pr_pids; do wait "$pr_p"; done
	cat "$D"/rr-s* "$D"/rr-g* > "$PT_OUT" 2>/dev/null
	awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) ~ /^SET/ { sr += unq($2); if (unq($5)+0 > sp50) sp50 = unq($5);
		                   if (unq($7)+0 > sp99) sp99 = unq($7) }
		unq($1) ~ /^GET/ { gr += unq($2); if (unq($5)+0 > gp50) gp50 = unq($5);
		                   if (unq($7)+0 > gp99) gp99 = unq($7) }
		END { if (sr == 0 || gr == 0) exit 1
		      printf "%.0f %.0f %s %s %s %s\n", sr, gr, sp50, gp50, sp99, gp99 }' \
		"$PT_OUT"
}

row() { # row <arm> <host> <port> <clients> <pipeline>
	a=$1; h=$2; p=$3; c=$4; pl=$5
	s_=""; g_=""; sp_=""; gp_=""; s9=""; g9=""; k=1
	# the first result is the pilot that sizes this cell (CELL_MIN_S);
	# the native dialect runs for a duration and is never sized
	rn=$(n_for "$pl" "$c"); rsz=""
	[ "${DIALECT:-resp}" = resp ] || rsz=nat
	while [ $k -le "$REPS" ]; do
		r=$(point "$h" "$p" "$c" "$pl" "$rn")
		if [ "$(printf '%s' "$r" | wc -w)" -eq 6 ]; then
			set -- $r
			if [ -z "$rsz" ]; then
				rsz=sized
				nn=$(cell_resize "$rn" "$(fastest "$1" "$2")")
				if [ -n "$nn" ]; then
					say "  $a c=$c P=$pl: pilot of $rn ran" \
						"$(cell_ms "$rn" "$(fastest "$1" "$2")") ms," \
						"under ${CELL_MIN_S}s - sized to $nn"
					rn=$nn
					continue           # a pilot is not a rep
				fi
			fi
			[ "$rsz" = nat ] || cell_floor "$a" "$c" "$pl" "$k" "$rn" "$(fastest "$1" "$2")"
			s_="$s_ $1"; g_="$g_ $2"; sp_="$sp_ $3"
			gp_="$gp_ $4"; s9="$s9 $5"; g9="$g9 $6"
		else
			say "  $a c=$c P=$pl rep $k: no result$(point_why)"
		fi
		k=$((k + 1))
	done
	[ -n "$s_" ] || { say "  $a c=$c P=$pl: NO RESULT in $REPS rep(s)$(point_why)"; return 0; }
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$a" "$c" "$pl" \
		"$(echo $s_ | med)" "$(echo $g_ | med)" "$(echo $sp_ | med)" \
		"$(echo $gp_ | med)" "$(echo $s9 | med)" "$(echo $g9 | med)" \
		>> "$D/results.tsv"
	msets=$(echo $s_ | med); mgets=$(echo $g_ | med)
	[ "$rsz" = nat ] || cell_rec "$a" "$c" "$pl" "$rn" "$msets" "$mgets"
	[ "$a" = redis ] && ref_put "$c" "$pl" "$msets" "$mgets"
	trow "$a" "$c" "$pl" "$msets" "$mgets" \
		"$(echo $s9 | med)" "$(echo $g9 | med)"
}

# GET-only, for the cold-entry arms: a SET through a cold proxy-mode node would place the key there
# and warm the node whose coldness is measured
point_get() { # point_get <host> <port> <clients> <pipeline> [requests]
	pt_files
	case ${DIALECT:-resp} in
	bin)  point_nat_get "$1" "$3" "$4"; return $? ;;
	resp) ;;
	*) say "  BUG: unknown DIALECT '$DIALECT' reached point_get()"; return 1 ;;
	esac
	bench_cli -h "$1" -p "$2" -t get -n "${5:-$(n_for "$4" "$3")}" -c "$3" \
		--threads "$(cli_threads "$3")" \
		-P "$4" -r "$KEYSPACE" -d "$VAL" --csv \
		>"$PT_OUT" 2>"$PT_ERR"
	awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) == "GET" { gr = unq($2); g5 = unq($5); g9 = unq($7) }
		END { if (gr == "") exit 1; printf "%.0f %s %s\n", gr, g5, g9 }' \
		"$PT_OUT"
}

row_get() { # row_get <arm> <host> <port> <clients> <pipeline>
	ga=$1; gh=$2; gpt=$3; gc=$4; gpl=$5
	gr_=""; g5_=""; g9_=""; gk=1
	gn=$(n_for "$gpl" "$gc"); gsz=""; gwarm=""
	[ "${DIALECT:-resp}" = resp ] || gsz=nat
	while [ $gk -le "$REPS" ]; do
		# rep 1 arrives cold; every later read (a CELL_MIN_S pilot too) must be re-cooled, or the
		# median is a warm number wearing a cold label
		if [ -n "$gwarm" ] && [ -n "$RECOOL" ]; then
			$RECOOL || break
		fi
		rg=$(point_get "$gh" "$gpt" "$gc" "$gpl" "$gn")
		gwarm=1
		if [ "$(printf '%s' "$rg" | wc -w)" -eq 3 ]; then
			set -- $rg
			if [ -z "$gsz" ]; then
				gsz=sized
				nn=$(cell_resize "$gn" "$1")
				if [ -n "$nn" ]; then
					say "  $ga c=$gc P=$gpl: pilot of $gn ran" \
						"$(cell_ms "$gn" "$1") ms, under ${CELL_MIN_S}s -" \
						"sized to $nn"
					gn=$nn
					continue           # a pilot is not a rep
				fi
			fi
			[ "$gsz" = nat ] || cell_floor "$ga" "$gc" "$gpl" "$gk" "$gn" "$1"
			gr_="$gr_ $1"; g5_="$g5_ $2"; g9_="$g9_ $3"
		fi
		gk=$((gk + 1))
	done
	[ -n "$gr_" ] || { say "  $ga c=$gc P=$gpl: no result$(point_why)"; return 0; }
	mg=$(echo $gr_ | med)
	[ "$gsz" = nat ] || cell_rec "$ga" "$gc" "$gpl" "$gn" "" "$mg"
	# SET columns empty: this arm does not write
	printf '%s\t%s\t%s\t\t%s\t\t%s\t\t%s\n' "$ga" "$gc" "$gpl" \
		"$mg" "$(echo $g5_ | med)" "$(echo $g9_ | med)" \
		>> "$D/results.tsv"
	trow "$ga" "$gc" "$gpl" "" "$mg" "" "$(echo $g9_ | med)"
}

# recool_node2 empties node 2 with rm + start_node, not `restart`: a restarted container comes back
# on a new veth without the netem qdisc.  Called once per cold rep.
RECOOL=""                              # set only while a cold arm runs
RECOOL_MODE=""
recool_node2() {
	say "  re-cooling node 2"
	$RT rm -f cb-n2 >/dev/null 2>&1
	start_node cb-n2 "$(n_ip 2)" "$D/n2-$RECOOL_MODE.conf" || {
		say "  node 2 did not come back - SKIPPING the rest of this"
		say "  cold arm rather than reporting a warm node as cold"
		return 1; }
	rc_k=0; rc_peers=0
	while [ $rc_k -lt 30 ]; do
		rc_peers=$($RT run --rm --network "$NET" \
			--entrypoint perfcli "$IMG" \
			-h "$(n_ip 1)" -p 6479 -a "$SECRET" \
			-j '{"method":"members"}' 2>/dev/null |
			grep -o '"node"' | wc -l)
		[ "${rc_peers:-0}" -ge 3 ] && break
		sleep 2; rc_k=$((rc_k + 2))
	done
	if [ "${rc_peers:-0}" -lt 3 ]; then
		say "  membership did not re-form - SKIPPING rather than"
		say "  printing a number from a two-node cluster"
		return 1
	fi
	await_ready "$(n_ip 2)" || {
		say "  node 2 is not READY - SKIPPING rather than reading"
		say "  through a node that refuses every data verb (S137)"
		return 1; }
	sleep 32                       # the reshard grace, again
	return 0
}

drive_get() { # drive_get <arm> <host> <port> [dialect]
	for dl in ${4:-$(arm_dialects "$1")}; do
		DIALECT=$dl; dn=$(arm_name "$1" "$dl")
		for c in $CLIENTS; do row_get "$dn" "$2" "$3" "$c" 1; done
		for pl in $PIPES; do
			[ "$pl" = 1 ] || row_get "$dn" "$2" "$3" "$PIPE_CLIENTS" "$pl"
		done
	done
	DIALECT=resp
}

# one pass per dialect, each its own arm; the redis arm is always resp, redis-benchmark being the
# only client that speaks to it
arm_dialects() { # arm_dialects <arm>
	[ "$1" = redis ] && { echo resp; return 0; }
	echo "$DIALECTS"
}
arm_name() { # arm_name <arm> <dialect>
	[ "$2" = resp ] && echo "$1" || echo "$1-$2"
}
drive_routed() { # drive_routed <arm> - RESP only, one client per owner
	DIALECT=resp; ROUTED_MODE=1
	for c in $CLIENTS; do row "$1" "$(n_ip 1)" "$RESP_PORT" "$c" 1; done
	for pl in $PIPES; do
		[ "$pl" = 1 ] || row "$1" "$(n_ip 1)" "$RESP_PORT" "$PIPE_CLIENTS" "$pl"
	done
	ROUTED_MODE=0
}
drive() { # drive <arm> <host> <port>
	for dl in $(arm_dialects "$1"); do
		DIALECT=$dl; dn=$(arm_name "$1" "$dl")
		for c in $CLIENTS; do row "$dn" "$2" "$3" "$c" 1; done
		for pl in $PIPES; do
			[ "$pl" = 1 ] || row "$dn" "$2" "$3" "$PIPE_CLIENTS" "$pl"
		done
	done
	DIALECT=resp
}

# ---- servers --------------------------------------------------------
start_redis() {
	$RT run -d --name cb-redis --network "$NET" --ip "$R_IP" $PIN_SRV \
		"$REDIS_IMG" --save '' --appendonly no >/dev/null 2>&1 || return 1
	shape cb-redis
	pin_check cb-redis "$CPUSET_SRV"
	k=0
	while [ $k -lt 60 ]; do
		$RT run --rm --network "$NET" --entrypoint redis-cli \
			"$REDIS_IMG" -h "$R_IP" PING 2>/dev/null | grep -q PONG && {
			shape_selftest "$R_IP" "$R_PORT"; return 0; }
		sleep 1; k=$((k + 1))
	done
	return 1
}

mkconf() { # mkconf <file> <advertise-ip|""> <mode-block>
	{
	cat <<EOF
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = $ARENA
[secrets]
client = $SECRET
cluster = cb-cluster-secret
[listen]
tcp = 0.0.0.0:6479
resp = 0.0.0.0:$RESP_PORT
# a non-loopback RESP listener needs an allow-list (RESP has no handshake);
# scoped to the bench subnet, which only the containers reach
resp_allow = $SUBNET
EOF
	if [ -n "$2" ]; then
		cat <<EOF
[cluster]
multicast = 239.98.0.1:6480
advertise = $2
pull_timeout_ms = 400
$3
$CLUSTER_EXTRA
collections = 0
EOF
	fi
	cat <<EOF
[collection 0]
buckets_log2 = 17
EOF
	} > "$1"
}

start_node() { # start_node <name> <ip> <conf>
	# --ulimit memlock=-1: the container default is far below the arena, so mlock() fails and the
	# arena runs unpinned
	$RT run -d --name "$1" --network "$NET" --ip "$2" $PIN_SRV \
		--ulimit memlock=-1 \
		-v "$3:/etc/perfcached.conf:ro" "$IMG" >/dev/null 2>&1 || return 1
	shape "$1"
	pin_check "$1" "$CPUSET_SRV"
	# ask the service, not the logs: a detached container may capture no logs at all
	k=0
	while [ $k -lt 60 ]; do
		if $RT run --rm --network "$NET" --entrypoint redis-cli \
		        "$REDIS_IMG" -h "$2" -p "$RESP_PORT" PING 2>/dev/null |
		        grep -q PONG; then
			return 0
		fi
		# a container that exited is never going to answer
		st=$($RT inspect -f '{{.State.Status}}' "$1" 2>/dev/null)
		[ "$st" = exited ] && break
		sleep 1; k=$((k + 1))
	done
	# show what the daemon said (from the stopped container's logs) and its config, the usual culprit
	say "  $1 did not become ready (status: $($RT inspect -f '{{.State.Status}}' "$1" 2>/dev/null))."
	say "  Daemon output (may be empty - some runtimes capture nothing"
	say "  from a detached container; run it in the foreground to see):"
	nl=$($RT logs "$1" 2>&1)
	printf '%s\n' "$nl" | tail -8 | sed 's/^/    /' >&2
	explain_build_failure "$nl" >&2
	# a node that died right after the memory probe almost certainly
	# could not get its arena, and the pool is host-wide
	case "$nl" in *"probed tier"*)
		say "  host huge pages:"
		grep -i huge /proc/meminfo 2>/dev/null | sed 's/^/    /' >&2 ;;
	esac
	say "  config was $3:"
	sed 's/^/    /' "$3" >&2
	return 1
}

# a node that answers PING can still refuse data verbs (an eager member RECOVERING until its bootstrap
# pull completes), so each node's own lifecycle state is awaited before an arm is driven
READY_WAIT_S=${READY_WAIT_S:-60}
node_state() { # node_state <ip> - the node's own lifecycle state, from stats
	$RT run --rm --network "$NET" --entrypoint perfcli "$IMG" \
		-h "$1" -p 6479 -a "$SECRET" -j '{"method":"stats"}' 2>/dev/null |
		grep -o '"state":"[a-z]*"' | head -1 | cut -d'"' -f4
}
await_ready() { # await_ready <ip>... - until every node reports "ready"
	ar_k=0
	while [ $ar_k -lt "$READY_WAIT_S" ]; do
		ar_not=""
		for ar_ip in "$@"; do
			ar_st=$(node_state "$ar_ip")
			[ "$ar_st" = ready ] || ar_not="$ar_not $ar_ip=${ar_st:-no-answer}"
		done
		if [ -z "$ar_not" ]; then
			[ $ar_k -eq 0 ] || say "  every node READY after ${ar_k}s"
			return 0
		fi
		sleep 1; ar_k=$((ar_k + 1))
	done
	say "  not READY after ${ar_k}s:$ar_not"
	return 1
}

# ---- PHASE 1: one against one --------------------------------------
if have_phase 1; then
	say "=== phase 1: redis vs ONE standalone perfcached ==="
	start_redis || { say "redis container failed to start"; exit 1; }
	drive redis "$R_IP" "$R_PORT"
	$RT rm -f cb-redis >/dev/null 2>&1

	mkconf "$D/solo.conf" "" ""
	start_node cb-solo "$S_IP" "$D/solo.conf" || exit 1
	if await_ready "$S_IP"; then
		drive perfcached-solo "$S_IP" "$RESP_PORT"
	else
		say "  skipping the standalone arm rather than driving a node"
		say "  that refuses every data verb (-LOADING)"
	fi
	$RT rm -f cb-solo >/dev/null 2>&1
fi

# ---- PHASE 2: one against three, per mode ---------------------------
if have_phase 2; then
	say "=== phase 2: redis vs a 3-node cluster, one arm per mode ==="
	if ! have_phase 1; then
		start_redis || { say "redis container failed to start"; exit 1; }
		drive redis "$R_IP" "$R_PORT"
		$RT rm -f cb-redis >/dev/null 2>&1
	fi
	for M in $MODEARMS; do
		case $M in
		store)  BLK="mode = store" ;;
		eager)  BLK="mode = eager" ;;
		proxy)  BLK="mode = proxy" ;;
		shard)  BLK="mode = shard" ;;
		spread) BLK="mode = spread
replicas = 2" ;;             # S127: K=2 of the three - through a plain
                             # client, so a read at a non-holder pulls
		*) say "unknown mode arm '$M'"; continue ;;
		esac
		say "--- 3-node cluster, mode=$M ---"
		ok=1
		for i in 1 2 3; do
			mkconf "$D/n$i-$M.conf" "$(n_ip $i)" "$BLK"
			start_node "cb-n$i" "$(n_ip $i)" "$D/n$i-$M.conf" || ok=0
		done
		if [ "$ok" = 1 ]; then
			# membership must actually form, or the "cluster" arm
			# is three unrelated daemons and the number is a lie
			k=0; peers=0
			while [ $k -lt 30 ]; do
				peers=$($RT run --rm --network "$NET" \
					--entrypoint perfcli "$IMG" \
					-h "$(n_ip 1)" -p 6479 -a "$SECRET" \
					-j '{"method":"members"}' 2>/dev/null |
					grep -o '"node"' | wc -l)
				[ "${peers:-0}" -ge 3 ] && break
				sleep 2; k=$((k + 2))
			done
			if [ "${peers:-0}" -lt 3 ]; then
				say "  MEMBERSHIP DID NOT FORM (saw $peers of 3)"
				say "  the bridge may not carry multicast - skipping mode=$M"
			elif say "  membership formed in ${k}s ($peers nodes)" &&
			     ! await_ready "$(n_ip 1)" "$(n_ip 2)" "$(n_ip 3)"; then
				# a member can be in the map and still RECOVERING
				say "  skipping mode=$M rather than driving a node"
				say "  that refuses every data verb (-LOADING)"
			else
				FLEET_UP=$(date +%s)
				drive "perfcached-$M" "$(n_ip 1)" "$RESP_PORT"
				# the same fleet through a cluster-aware Redis client, for the modes
				# with an owner to compute (ROUTED=0 skips it)
				case $M in
				shard|spread)
					if [ "${ROUTED:-1}" = 1 ] && routed_tags; then
						drive_routed "perfcached-$M-routed"
					fi ;;
				esac

				# cold entry: read through a node holding nothing, so the arm measures
				# the mode, not placement; after SHARD_GRACE_S, inside which misses broadcast
				up=$(( $(date +%s) - FLEET_UP ))
				if [ "$up" -lt 32 ]; then
					say "  (waiting $((32 - up))s for the reshard grace)"
					sleep $((32 - up))
				fi
				bench_cli -h "$(n_ip 1)" -p "$RESP_PORT" \
					-t set -n "$N" -c 20 \
					--threads "$(cli_threads 20)" \
					-r "$KEYSPACE" \
					-d "$VAL" --csv >/dev/null 2>&1
				# every cold rep re-cools node 2 (RECOOL).  Never routed: a routed client
				# reads each key's owner, so emptying node 2 deletes a third of the keys.
				COLD_SAVED_ROUTE=$ROUTE
				ROUTE=0
				RECOOL_MODE=$M
				RECOOL=recool_node2
				cold1=1
				for cdl in $(arm_dialects "perfcached-$M-cold"); do
					if [ "$cold1" = 0 ]; then
						recool_node2 || break
					fi
					drive_get "perfcached-$M-cold" "$(n_ip 2)" \
						"$RESP_PORT" "$cdl"
					cold1=0
				done
				RECOOL=""
				ROUTE=$COLD_SAVED_ROUTE
				# with N well above KEYSPACE a cold rep mostly reads what it just pulled:
				# this column measures the warm-up transient, not the cold cost
				say "  (cold arm: each rep re-reads the ${KEYSPACE}-key"
				say "  keyspace several times over, so only the first"
				say "  pass is cold - this column is a warm-up"
				say "  TRANSIENT, not the cold cost.  Set N near"
				say "  KEYSPACE to weight it toward cold reads.)"
			fi
		fi
		$RT rm -f cb-n1 cb-n2 cb-n3 >/dev/null 2>&1
	# huge pages are not returned the instant a container dies; wait for them back before the next mode
	sleep 3
	done
fi

say ""
say "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"

SUMDIR=$(dirname "$0")
[ -x "$SUMDIR/summarize.sh" ] && sh "$SUMDIR/summarize.sh" "$D/results.tsv"
