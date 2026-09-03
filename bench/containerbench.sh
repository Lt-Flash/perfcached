#!/bin/sh
# containerbench.sh — the RESP comparison, assembled from nothing.
#
# Builds its own perfcached image, pulls its own Redis, creates its own
# network, runs both phases, writes one TSV, tears everything down.  The
# host needs a container runtime and nothing else: no compiler, no
# libsodium, no redis-server, no perfcached binary.  That is the point -
# bench/respbench.sh measures the same thing but wants a built tree and
# a Redis on the host, which is a lot to ask of someone evaluating this
# for the first time.
#
#   PHASE 1  redis container  vs  ONE standalone perfcached container
#   PHASE 2  redis container  vs  a 3-node perfcached cluster, one arm
#            per mode: store, eager, proxy, shard
#
# THE CLIENT IS redis-benchmark, RUN IN ITS OWN CONTAINER, on the same
# bridge as both servers.  Redis's own tool, unmodified, speaking native
# RESP to redis-server and to perfcached's RESP door alike - so neither
# server is favoured by client placement, and the client cannot tell
# which it reached.  Running it on the host instead would give whichever
# server shares the host's network namespace an unearned advantage.
#
# Usage:
#   bench/containerbench.sh                 # everything, 3 dialects
#   REPS=1 bench/containerbench.sh          # smoke run, ~8 min
#   PHASES=1 bench/containerbench.sh        # standalone comparison only
#   MODEARMS="store shard" bench/containerbench.sh
#   DIALECTS=resp bench/containerbench.sh   # RESP only, the old behaviour
#   DIALECTS="resp bin" bench/containerbench.sh
#   RUNTIME=podman bench/containerbench.sh  # default: nerdctl, else podman
#   NETEM="delay 5ms rate 10mbit" bench/containerbench.sh
#                                           # shape the bench traffic
#
# Results: /var/tmp/containerbench/results.tsv, same COLUMNS as
# respbench.sh - but do not read the two as one table.  Everything here
# crosses a bridge veth and sits under a container's limits, where
# respbench runs on host loopback; the arms inside THIS file are
# comparable to each other, which is what it is for.  Comparing a
# container number to a host number measures the container.
set -u

# Pick a runtime that can actually BUILD, not merely one that is
# installed.  nerdctl answers `info` happily and then fails every build
# because buildkitd is not running - so "installed" and "usable" and
# "can do the job" are three different questions, and only the last one
# matters here.  Probed with a real one-line image rather than a flag.
RT=${RUNTIME:-}
# THE PROBE MUST RUN A BUILD STEP.  Two versions of this were wrong
# before this one:
#   `FROM scratch` alone     - valid for podman, REJECTED by Docker, so
#                              the probe failed on itself and blamed the
#                              runtime.
#   `FROM scratch` + LABEL   - builds under both, but starts NO
#                              container, so it passed on a host where
#                              AppArmor blocked every RUN step and the
#                              real build died at `RUN apk add`.
# A probe that does not exercise the path being probed is worse than no
# probe: it converts "this will fail" into "this will fail later, with a
# more confusing message".  So it now runs an actual step, using the
# same base image the real build uses - the pull is shared, not extra.
PROBE_ERR=""
BUILD_ENV=""

# do_build <runtime> <-f file> <args...> - one place that knows how to
# invoke a build, so the probe and the real build cannot diverge.
#
# Docker gets BUILDKIT FIRST.  The legacy builder starts each RUN step
# under the docker-default AppArmor profile, and on a host where that
# profile will not load ("Unable to replace docker-default. Profile
# doesn't conform to protocol") every RUN dies with exit 185 - dockerd
# healthy, images pullable, builds impossible.  BuildKit executes steps
# through its own path and is unaffected.  Legacy is kept as the
# fallback for daemons too old for DOCKER_BUILDKIT=1.
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
# MTU: default to whatever the host's DEFAULT-ROUTE interface uses, not
# the runtime's 1500.  A container bridge is 1500 unless told otherwise,
# and read throughput tracks MTU directly - a pipelined batch of 200 B
# GET responses is one segment at 65536, two at 9000, nine at 1500.
# Measured on one host: GET 502,673/s at 1500, 793,905 at 9000, and
# 1,639,869 on loopback, while SET barely moved.  A host whose NIC runs
# 9000 and whose bench ran at 1500 was measuring a network it does not
# have, and reporting its cache as slower than it is.
host_mtu() {
	hi=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
	[ -n "$hi" ] && ip link show "$hi" 2>/dev/null |
		sed -n 's/.*mtu \([0-9]*\).*/\1/p' | head -1
}
MTU=${MTU:-$(host_mtu)}
case "${MTU:-}" in ''|*[!0-9]*) MTU=1500 ;; esac
SUBNET=${SUBNET:-10.98.0.0/24}
IMG=${IMG:-perfcached:bench}
# Debian-based (glibc), never a musl tag.  Two reasons, and the second
# is the one that bit: it is the reference server, so it should share a
# base with the image under test; and cb-drive runs redis-benchmark FROM
# THIS IMAGE, so a musl tag means the resp arms are driven by a
# musl-built client while the native arms are not.  See the musl
# allocator note above - that is not a difference anyone wants inside a
# comparison.  +29 MB (119 -> 148).
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
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
REPS=${REPS:-3}
N=${N:-100000}
KEYSPACE=${KEYSPACE:-20000}
VAL=${VAL:-200}
# 128, not 512: phase 2 runs THREE nodes at once, and on a host with a
# RESERVED hugepage pool (nr_hugepages, as any HG_MALLOC/OpenSIPS box
# has) they come out of that pool rather than THP.  3 x 512 MB wants
# 1.5 GB of hugepages; node 1 takes its share and node 2 exits right
# after "probed tier = MAP_HUGETLB" with nothing left.  bench/
# containers-up.sh has used 128 for the same reason.
ARENA=${ARENA:-128}
# WORKERS: one epoll thread each.  Half the cores, because THE CLIENT
# SHARES THIS BOX - redis-benchmark is a real CPU consumer, and so are
# the daemon's own maintenance, WAL and RDB threads.  Giving the workers
# every core starves the client and the measurement stops being about
# the server.
#
# Measured on a 4-vCPU host, 50 clients, pipeline 64, redis stable to
# within 1.9% across the three runs:
#
#   workers   SET/s       GET/s       SET p99   GET p99
#   1         1,111,467   1,020,735   3.911ms   3.783ms
#   2         1,449,739   1,042,000   2.623ms   5.847ms
#   4         1,408,901     735,529   1.439ms  10.887ms
#
# The second worker is worth +30% on SET.  The fourth buys nothing and
# costs 28% of GET throughput while tripling its tail - GET replies
# carry the values, so the client needs a core to parse them and at
# workers=cores there is none left.  cores/2 was the best of the three,
# and it was also the old default's worst case.
#
# The README figures were taken with 4 workers; set WORKERS=4 to
# reproduce them on a >=8-core host.
NCPU=$(nproc 2>/dev/null || echo 2)
WORKERS=${WORKERS:-$([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)}

# CPUSET_SRV / CPUSET_CLI: put the servers and the client on separate
# CPUs.  Two different confounds hide in an unpinned run on a
# multi-socket box, and pinning both sides apart removes them together:
#
#   - the client competes with the workers for cores, so "more workers"
#     and "less client" move at the same time (measured: at workers =
#     cores on a 4-vCPU host, GET fell to x0.72 and its p99 tripled);
#   - the arena is allocated and first-touched at startup, so it lands
#     on ONE NUMA node.  A worker scheduled on the far socket pays
#     remote memory for every bucket it reads, which looks exactly like
#     "adding workers stopped helping".
#
# On a 2-socket host with node0 = 0-3 and node1 = 4-7:
#
#   CPUSET_SRV=0-3 MEMS_SRV=0 CPUSET_CLI=4-7 bench/containerbench.sh
#
# gives the servers four cores with all their memory local and hands the
# client the other socket, so neither effect is in the number.
CPUSET_SRV=${CPUSET_SRV:-}
CPUSET_CLI=${CPUSET_CLI:-}
MEMS_SRV=${MEMS_SRV:-}
PIN_SRV=""
[ -n "$CPUSET_SRV" ] && PIN_SRV="--cpuset-cpus=$CPUSET_SRV"
[ -n "$MEMS_SRV" ] && PIN_SRV="$PIN_SRV --cpuset-mems=$MEMS_SRV"
PIN_CLI=""
[ -n "$CPUSET_CLI" ] && PIN_CLI="--cpuset-cpus=$CPUSET_CLI"
PHASES=${PHASES:-"1 2"}
MODEARMS=${MODEARMS:-"store eager proxy shard"}
SECRET=cb-client-secret

# Dialects to drive each perfcached arm with.  redis-benchmark speaks
# only RESP, so the redis reference arm is always resp regardless of
# this - the native rows are compared against that same reference.
#
#   resp - the RESP door, driven by redis-benchmark (Redis's own build
#          of Redis's own tool: nothing of ours in the client path)
#   json - the native newline-JSON dialect, driven by natbench
#   bin  - the native binary dialect (16-byte header frames), same client
#
# json and bin both go through libperfd, which is the library a real
# client links, so those rows are what an application can actually get -
# pcbench's numbers are not, it counts frames in place and never
# materialises a value.
#
#   DIALECTS="resp bin" bench/containerbench.sh
DIALECTS=${DIALECTS:-"resp json bin"}
NAT_PORT=6479                          # the native listener; RESP is 6380
NAT_SECS=${NAT_SECS:-5}                # natbench runs for a DURATION
# Latency sampling: 1 = time every request, as redis-benchmark does, so
# the two clients carry the same instrument.  natbench measures that at
# ~25% of its own CPU at depth 256; raise this to trade percentiles for
# throughput, but then the resp and native rows are no longer paying the
# same cost and the comparison is not like-for-like.
NAT_LAT=${NAT_LAT:-1}
# redis-benchmark takes a REQUEST COUNT and natbench takes a DURATION,
# so at high pipelining the two clients were not running for comparable
# times: N=100000 at pipeline 16 finishes in about a QUARTER SECOND
# while the native arm ran a full NAT_SECS.  The tell in a results file
# is a redis row of exactly 100000/0.251 = 398,406 and 100000/0.502 =
# 199,203 - timer-quantised values from a sub-second run, and in one
# such run redis's GET went DOWN when given more threads, which is not
# a regression, it is a measurement that never reached steady state.
#
# Scale the count with the pipeline depth so every cell runs for roughly
# the same wall time.  N stays the pipeline-1 count, which is what it
# always meant.
# Scale the request count with BOTH knobs that raise throughput, or the
# cell finishes before it means anything.  Depth alone was not enough:
# at pipeline 1 the count stayed at N and 16-plus clients ran it in
# ~0.75s, which quantises on redis-benchmark's millisecond timer - the
# tell was 133,156 (= 100000/0.751) appearing IDENTICALLY across four
# different cells and two different arms.
#
# The client factor saturates because throughput does: measured 15,865/s
# at one client and ~133,000/s from sixteen clients upward, so past that
# more connections buy queueing, not rate.
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

# Container base.  GLIBC, and there is no second option: musl's
# allocator serialises libperfd's per-reply malloc on the JSON path, so
# a musl-built client peaks at TWO threads and then goes BACKWARDS -
# 217,391 ops/s at 1 thread, 42,104 at 8, where the same test on the
# glibc image climbs 339,555 -> 905,550.  A 21.5x gap at 8 threads that
# has nothing to do with the dialect, the wire or the daemon, and it
# silently made every containerised JSON figure this harness ever
# printed a measurement of the allocator.  The musl Containerfile was
# deleted rather than left as a knob someone could reach for.
CONTAINERFILE=${CONTAINERFILE:-bench/Containerfile.debian}
# Extra lines for every node's [cluster] block.  Exists so a run can
# provoke a specific refusal on purpose - e.g. max_pending = 64 to make
# the parked table overflow - which is how the error-capture path gets
# tested against a real failure instead of a hypothetical one.
CLUSTER_EXTRA=${CLUSTER_EXTRA:-}

# Per-key ROUTING for the native arms (S35), on by default because it is
# the configuration a cluster-aware deployment actually runs, and until
# e0a0a83 it was unreachable from the async API - so every shard number
# this harness ever printed was a client that could not route, paying a
# forward on nearly every key.  Measured on a 3-node fleet, shard went
# 222,528 -> 2,561,332 ops/s with forwards falling to zero.
#
# What it does NOT do is make the native arms comparable to the resp
# one.  A routed client spreads its connections over the whole fleet;
# redis-benchmark speaks only to node 1 and cannot route at all.  Those
# are different experiments - read json-vs-bin, and redis-vs-resp, and
# do not read resp-vs-native.
#
# ROUTE=0 reproduces the pre-e0a0a83 numbers.
ROUTE=${ROUTE:-1}

# Every list knob is WHITESPACE-separated, and a comma is the obvious
# typo.  Accept it - the intent is unambiguous - and then VALIDATE, so
# an unknown value cannot pass silently.  `DIALECTS=bin,json` used to
# reach point() as one token, match neither json nor bin, and fall
# through to the resp default: the run measured the RESP door and
# labelled the arm "perfcached-solo-bin,json".  A wrong number wearing
# the right name is the worst outcome a harness can produce.
list_norm() { printf '%s' "$1" | tr ',' ' '; }
CLIENTS=$(list_norm "$CLIENTS")
PIPES=$(list_norm "$PIPES")
PHASES=$(list_norm "$PHASES")
MODEARMS=$(list_norm "$MODEARMS")
DIALECTS=$(list_norm "$DIALECTS")

for dl_ in $DIALECTS; do
	case $dl_ in
	resp|json|bin) ;;
	*) echo "containerbench: unknown dialect '$dl_'" >&2
	   echo "  DIALECTS must be some of: resp json bin" >&2
	   exit 2 ;;
	esac
done
for m_ in $MODEARMS; do
	case $m_ in
	store|eager|proxy|shard) ;;
	*) echo "containerbench: unknown mode '$m_'" >&2
	   echo "  MODEARMS must be some of: store eager proxy shard" >&2
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
# Client threads, and it must be the SAME budget for both clients or the
# arms are not comparable.  redis-benchmark is SINGLE-threaded unless
# told otherwise, while natbench takes a thread count - so a run that
# passed one and not the other gave the native arms N client threads and
# the resp arms one, and the native-vs-redis ratio carried the
# difference.  The tell was that the redis arm and the perfcached RESP
# arm tracked each other closely (42,680 vs 39,872 SET/s at 50 clients)
# while the native arms sat 4x higher on the same host.
#
# Both drivers now take this, capped at the client count (neither client
# can use more threads than it has connections).
CLI_THREADS=${CLI_THREADS:-$([ "$NCPU" -ge 2 ] && echo $((NCPU / 2)) || echo 1)}
NAT_THREADS=${NAT_THREADS:-}

# 10.98.0.10 redis | .11 standalone perfcached | .21-.23 the cluster
R_IP=10.98.0.10;  R_PORT=6379
S_IP=10.98.0.11
n_ip() { echo "10.98.0.2$1"; }
RESP_PORT=6380

say() { echo "$@" >&2; }
have_phase() { case " $PHASES " in *" $1 "*) return 0;; esac; return 1; }

# NET_KEEP=1: use an EXISTING network and do not remove it.  Without
# this there is no way to shape the bench traffic, because the script
# creates its bridge at start and deletes it at exit - so a qdisc
# attached beforehand goes with it, and one attached to docker0 shapes
# an interface that is not in the path at all.  A 10mbit netem on
# docker0 left a run reporting 909,382 GET/s of 200-byte values, which
# is ~180 MB/s down a 1.25 MB/s link: the tell that it was shaping
# nothing.
#
#   docker network create --subnet 10.98.0.0/24 pcbenchnet
#   BR=br-$(docker network inspect pcbenchnet -f "{{.Id}}" | cut -c1-12)
#   tc qdisc add dev $BR root netem delay 5ms rate 10mbit
#   NET_KEEP=1 bench/containerbench.sh
#   tc qdisc del dev $BR root
NET_KEEP=${NET_KEEP:-0}

# NETEM="delay 5ms rate 10mbit" - shape the bench traffic.
#
# This has to be applied to each container's HOST-SIDE VETH, and that is
# not obvious: container-to-container traffic is forwarded between veth
# ports by the bridge and NEVER passes the bridge device's root qdisc,
# so `tc qdisc add dev br-xxxx` shapes nothing.  Neither does docker0,
# which is not even in the path.  Both were tried: a "10mbit" limit left
# a run reporting 862,345 GET/s of 200-byte values - about 172 MB/s down
# a 1.25 MB/s link - and nothing in the output said the shaping was
# inert.
#
# The veths only exist while a container runs, so the script attaches
# the qdisc itself after each start.
#
# SEMANTICS: this is a root qdisc on the HOST side of a veth, so it
# shapes ONE DIRECTION - packets on their way INTO that container.  A
# path is symmetric only when both of its endpoints are shaped, which
# is why the client now lives in a persistent, named, shaped container
# (drive_up) instead of an ephemeral `run --rm` that has no veth to
# attach anything to.  While the client was unshaped its replies came
# home at full speed, so "delay 5ms" produced 5ms of ROUND TRIP rather
# than 10, every single-hop arm landed on the same throughput, and the
# run read as a dead heat between servers when it was only ever
# measuring the pipe.  shape_selftest() measures the round trip and
# says so out loud if it comes back at 1x.
#
# So: `delay` is PER DIRECTION and the RTT is twice it; `rate` is per
# direction as well, i.e. full duplex.
NETEM=${NETEM:-}

host_veth() { # host_veth <container> -> the veth on THIS side of it
	hv_pid=$($RT inspect -f '{{.State.Pid}}' "$1" 2>/dev/null)
	[ -n "$hv_pid" ] || return 1
	# `ip link`, NOT /sys/class/net/eth0/iflink.  SYSFS DOES NOT FOLLOW
	# THE NETWORK NAMESPACE: it reflects the netns that mounted it, so
	# reading it under `nsenter -n` returns the HOST's interfaces and
	# the peer lookup silently matches nothing.  ip(8) uses netlink,
	# which does respect the namespace.  The container side prints as
	# `eth0@if12`, and 12 is the host-side index.
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
	# `replace`, not `add`: under NET_KEEP=1 a veth can come back with a
	# qdisc still on it, and `add` then fails EEXIST - which the old code
	# swallowed into "tc failed", printing a failure while the link was
	# in fact shaped.
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

# The client has to be shaped like everything else, or the whole reply
# direction is free.  That needs a container which exists BEFORE any
# traffic starts and keeps one veth across every measurement, so the
# client is started once and each measurement is an exec into it.
drive_up() {
	$RT inspect -f '{{.State.Running}}' cb-drive 2>/dev/null | grep -q true || {
		$RT rm -f cb-drive >/dev/null 2>&1
		$RT run -d --name cb-drive --network "$NET" $PIN_CLI \
			--entrypoint tail \
			"$REDIS_IMG" -f /dev/null >/dev/null 2>&1 || {
			say "  could not start the client container cb-drive"
			return 1; }
	}
	# unconditionally, even for a container that was already up: one left
	# behind by a killed run carries THAT run's qdisc, or none, and an
	# unshaped client is precisely the bug this function exists to fix.
	# shape() uses `tc qdisc replace`, so re-shaping is free.
	shape cb-drive
	pin_check cb-drive "$CPUSET_CLI"
}

bench_cli() { $RT exec cb-drive redis-benchmark "$@"; }

# The native dialects need OUR image (natbench links libperfd), and the
# redis image obviously does not carry it - so the native arms get their
# own persistent client container.  It is shaped and pinned by exactly
# the same calls as cb-drive: a native arm driven from an UNSHAPED
# client while the resp arm is shaped would compare two different
# networks and read as a dialect result, which is the same class of bug
# as shaping one direction only.
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

# nat_run <host> <conns> <depth> <bin 0|1> <getpct> -> "rps p50 p99"
# Fails (exit 1) on no result, on a run that reported errors, and on the
# zero-hit case - a GET pass that read an empty keyspace would otherwise
# report a splendid rps for returning nothing.
nat_run() {
	pt_files
	nat_cli "$1" "$NAT_PORT" "$SECRET" 0 "$2" "$(nat_threads "$2")" \
		"$3" "$NAT_SECS" "$KEYSPACE" "$VAL" "$5" "$4" "$NAT_LAT" \
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

# Two passes per point - getpct 0 then 100 - so the SET and GET columns
# mean what they mean for redis-benchmark, and the GET pass runs second,
# reading keys the SET pass just wrote.
point_nat() { # point_nat <host> <clients> <depth> <bin 0|1>
	pn_s=$(nat_run "$1" "$2" "$3" "$4" 0)   || return 1
	pn_g=$(nat_run "$1" "$2" "$3" "$4" 100) || return 1
	set -- $pn_s; pn_sr=$1; pn_s5=$2; pn_s9=$3
	set -- $pn_g; pn_gr=$1; pn_g5=$2; pn_g9=$3
	printf '%s %s %s %s %s %s\n' "$pn_sr" "$pn_gr" "$pn_s5" "$pn_g5" \
		"$pn_s9" "$pn_g9"
}

point_nat_get() { # point_nat_get <host> <clients> <depth> <bin 0|1>
	nat_run "$1" "$2" "$3" "$4" 100
}

# Read the pinning back.  `--opt mtu=` was silently ignored by docker
# earlier in this harness's life and nothing said so; an unverified knob
# is worse than no knob, because the run still prints a number.
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

# Prove the shaping bites in BOTH directions.  A one-way netem is not a
# visible failure - it silently halves the round trip - so measure it:
# with `delay Dms` at each end a PING must come back at ~2D, and
# anything near 1D means the reply path is still free.
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

# The image must be able to name the binary inside it, for the same
# reason every other harness here refuses an unstamped build: a results
# file that cannot say what produced it is not a measurement.
BINREV=$($RT run --rm --entrypoint perfcached "$IMG" -V 2>/dev/null |
	sed -n 's/.*(\(.*\)).*/\1/p')
case "${BINREV:-unknown}" in
unknown|"") say "containerbench: image reports no revision - refusing"; exit 2;;
esac
say "    image built: perfcached $BINREV"

cleanup
# The MTU option is NOT spelled the same everywhere.  podman and
# nerdctl take `--opt mtu=`; docker wants the bridge driver's own key,
# `com.docker.network.driver.mtu`, and SILENTLY IGNORES the short form -
# it creates the network at 1500 and reports success.  That is exactly
# why the check below reads the MTU back from a container instead of
# trusting the exit status: a host whose NIC runs 9000 was benchmarking
# reads across 1500 and had no way to tell.
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
# sysfs, not `ip link`: iproute2 is NOT installed in debian-slim, while
# the busybox image this harness used to run did provide it - so moving
# REDIS_IMG to the Debian build silently turned this into an empty
# string and printed `mtu=?` into the provenance line.  /sys/class/net
# exists in every image.
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
case " $DIALECTS " in *" json "*|*" bin "*) drive_nat_up || exit 1 ;; esac

printf '# build=%s date=%s host=%s runtime=%s cpus=%s workers=%s arena_mb=%s keys=%s val=%s reps=%s mtu=%s cpuset_srv=%s cpuset_cli=%s mems_srv=%s dialects=%s nat_secs=%s nat_lat=%s cli_threads=%s n_max=%s base=%s redis_img=%s route=%s\n' \
	"$BINREV" "$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$RT" \
	"$NCPU" "$WORKERS" "$ARENA" "$N" "$VAL" "$REPS" "${ACT_MTU:-?}" \
	"${CPUSET_SRV:-none}" "${CPUSET_CLI:-none}" "${MEMS_SRV:-none}" \
	"$(echo $DIALECTS | tr ' ' ',')" "$NAT_SECS" "$NAT_LAT" "$CLI_THREADS" "$N_MAX" "$(basename "$CONTAINERFILE" | sed 's/^Containerfile\.//')" \
	"$(basename "$REDIS_IMG")" "$ROUTE" \
	> "$D/results.tsv"
printf 'arm\tclients\tpipeline\tset_rps\tget_rps\tset_p50ms\tget_p50ms\tset_p99ms\tget_p99ms\n' \
	>> "$D/results.tsv"

# ---- the driver -----------------------------------------------------
# redis-benchmark ships in the redis image, so the client is Redis's own
# build of Redis's own tool - nothing of ours is in the measurement path.
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

# $DIALECT selects the client and the wire; the row/median/REPS logic
# above it is shared, so a native row is built exactly like a resp one.
# Why the last point produced nothing.  The client's stderr used to go
# to /dev/null, so a failed cell reported "no result" and NOTHING else -
# and there are now four distinct reasons a cluster request is refused
# (TRYAGAIN, forward failed, holder rejected, holder timed out) which
# that discarded byte was the only thing distinguishing.  A run that
# says a cell died without saying why costs another whole run.
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

point() { # point <host> <port> <clients> <pipeline>
	pt_files
	case ${DIALECT:-resp} in
	json) point_nat "$1" "$3" "$4" 0; return $? ;;
	bin)  point_nat "$1" "$3" "$4" 1; return $? ;;
	resp) ;;
	*) say "  BUG: unknown DIALECT '$DIALECT' reached point()"; return 1 ;;
	esac
	bench_cli -h "$1" -p "$2" -t set,get -n "$(n_for "$4" "$3")" -c "$3" \
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

row() { # row <arm> <host> <port> <clients> <pipeline>
	a=$1; h=$2; p=$3; c=$4; pl=$5
	s_=""; g_=""; sp_=""; gp_=""; s9=""; g9=""; k=1
	while [ $k -le "$REPS" ]; do
		r=$(point "$h" "$p" "$c" "$pl")
		if [ "$(printf '%s' "$r" | wc -w)" -eq 6 ]; then
			set -- $r
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
	[ "$a" = redis ] && ref_put "$c" "$pl" "$msets" "$mgets"
	trow "$a" "$c" "$pl" "$msets" "$mgets" \
		"$(echo $s9 | med)" "$(echo $g9 | med)"
}

# GET-only, for the cold-entry arms.  Reads only, and not as a
# shortcut: a SET through a cold node in PROXY mode would place the key
# THERE (the self-preference band), warming the very node whose coldness
# is the measurement.
point_get() { # point_get <host> <port> <clients> <pipeline>
	pt_files
	case ${DIALECT:-resp} in
	json) point_nat_get "$1" "$3" "$4" 0; return $? ;;
	bin)  point_nat_get "$1" "$3" "$4" 1; return $? ;;
	resp) ;;
	*) say "  BUG: unknown DIALECT '$DIALECT' reached point_get()"; return 1 ;;
	esac
	bench_cli -h "$1" -p "$2" -t get -n "$(n_for "$4" "$3")" -c "$3" \
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
	while [ $gk -le "$REPS" ]; do
		# rep 1 arrives cold; every rep after it must be put back,
		# or the median is a warm number wearing a cold label
		if [ $gk -gt 1 ] && [ -n "$RECOOL" ]; then
			$RECOOL || break
		fi
		rg=$(point_get "$gh" "$gpt" "$gc" "$gpl")
		if [ "$(printf '%s' "$rg" | wc -w)" -eq 3 ]; then
			set -- $rg
			gr_="$gr_ $1"; g5_="$g5_ $2"; g9_="$g9_ $3"
		fi
		gk=$((gk + 1))
	done
	[ -n "$gr_" ] || { say "  $ga c=$gc P=$gpl: no result$(point_why)"; return 0; }
	mg=$(echo $gr_ | med)
	# SET columns empty: this arm does not write
	printf '%s\t%s\t%s\t\t%s\t\t%s\t\t%s\n' "$ga" "$gc" "$gpl" \
		"$mg" "$(echo $g5_ | med)" "$(echo $g9_ | med)" \
		>> "$D/results.tsv"
	trow "$ga" "$gc" "$gpl" "" "$mg" "" "$(echo $g9_ | med)"
}

# <dialect> optional: drive ONE dialect.  The cold arm needs that,
# because a cold read warms the node it read through - the second
# dialect through the same node would not be cold and would look
# faster for a reason that has nothing to do with the wire.
# Put node 2 back to genuinely empty.  rm+start_node, NOT `restart`: a
# restarted container comes back on a NEW veth, so the netem qdisc
# shape() put on the old one is gone and this arm would run unshaped
# while the others did not.  start_node re-shapes, re-pins and waits for
# the port.  Called once per COLD REP - see RECOOL in row_get.
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

# One pass per dialect, each its own arm.  The redis arm is forced to
# resp: redis-benchmark is the only client that speaks to it, and an
# arm named "redis-bin" would be a lie about what was measured.
arm_dialects() { # arm_dialects <arm>
	[ "$1" = redis ] && { echo resp; return 0; }
	echo "$DIALECTS"
}
arm_name() { # arm_name <arm> <dialect>
	[ "$2" = resp ] && echo "$1" || echo "$1-$2"
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
# S33: the RESP dialect has no handshake, so a non-loopback listener is
# REFUSED without an allow-list - the daemon will not start otherwise,
# which is how this harness found out.  Scoped to the bench subnet: the
# containers are the only things that can reach it.
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
	# --ulimit memlock=-1: containers default to a memlock limit far
	# below the arena, so mlock() fails and the arena runs UNPINNED
	# (swappable) - a property of the container, not of the daemon, and
	# one that would quietly show up as "perfcached is slower in
	# containers".  Observed here before it was raised.
	$RT run -d --name "$1" --network "$NET" --ip "$2" $PIN_SRV \
		--ulimit memlock=-1 \
		-v "$3:/etc/perfcached.conf:ro" "$IMG" >/dev/null 2>&1 || return 1
	shape "$1"
	pin_check "$1" "$CPUSET_SRV"
	# ASK THE SERVICE, do not read the logs.  A detached container here
	# runs perfectly and `podman logs` returns nothing at all - the
	# daemon is healthy and the log line the old check waited for never
	# arrives, so it timed out on a working node.  Whether logs are
	# captured depends on the runtime's log driver; whether the port
	# answers is the thing actually being waited for.
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
	# print what the daemon actually said.  A container that refuses to
	# start says WHY on stderr and then exits, so the useful output is
	# in the logs of a stopped container - fetch them before anything
	# removes it, and show the config too, since every failure so far
	# has been the config rather than the image.
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

# ---- PHASE 1: one against one --------------------------------------
if have_phase 1; then
	say "=== phase 1: redis vs ONE standalone perfcached ==="
	start_redis || { say "redis container failed to start"; exit 1; }
	drive redis "$R_IP" "$R_PORT"
	$RT rm -f cb-redis >/dev/null 2>&1

	mkconf "$D/solo.conf" "" ""
	start_node cb-solo "$S_IP" "$D/solo.conf" || exit 1
	drive perfcached-solo "$S_IP" "$RESP_PORT"
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
		eager)  BLK="mode = store
eager = 1" ;;
		proxy)  BLK="mode = proxy" ;;
		shard)  BLK="mode = shard" ;;
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
			else
				say "  membership formed in ${k}s ($peers nodes)"
				FLEET_UP=$(date +%s)
				drive "perfcached-$M" "$(n_ip 1)" "$RESP_PORT"

				# COLD ENTRY.  Driving every mode through node 1
				# compares a mode at a 100% local hit rate against
				# one at 33%, which measures the PLACEMENT and not
				# the mode: proxy keeps every key on the node that
				# received it, shard cannot.  Read through a node
				# holding nothing and shard wins 2.1x with an 8.7x
				# tighter tail, because it COMPUTES the owner and
				# unicasts while proxy consults a locator and
				# broadcasts when that misses.
				#
				# Wait out SHARD_GRACE_S first: for 30s after a
				# membership change a shard miss does not answer
				# authoritatively, it retries once as a BROADCAST.
				# Measured inside that window shard looks slower
				# than proxy; measured after it, faster.  Two
				# ad-hoc runs disagreed for exactly this reason.
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
				# One dialect per pass, and node 2 is RESTARTED
				# between them: the first pass pulls keys onto
				# it, so a second dialect reading the same node
				# would be reading a warm one.  natbench and
				# redis-benchmark share the key format
				# (key:%012u), so all three dialects read the
				# keys the fill above wrote.
				# EVERY cold rep must be cold.  Re-cooling once
				# per dialect leaves rep 1 cold and reps 2..N
				# reading the node rep 1 just warmed, so the
				# median REPS reports is a warm number wearing
				# a cold label - store-cold matched store warm
				# to seven digits before this.
				# The cold arm is a NON-ROUTING concept and must
				# not be run routed.  It empties node 2 and reads,
				# which is exactly what a RESP client meets: it
				# lands on a node that does not hold the key and
				# the daemon has to fetch it.  A ROUTED client
				# never lands on the wrong node - it reads from
				# each key's owner - so emptying node 2 does not
				# make reads cold, it DELETES a third of the
				# keyspace.  At 1-4 connections every one can sit
				# on the emptied member and the arm reports ZERO
				# HITS; at 16+ the misses hide inside a ~2/3 hit
				# rate, which is worse, because then it looks
				# like a number.
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
				# a cold rep only reads COLD keys until it has
				# walked the keyspace once; after that it is
				# reading what it just pulled.  With N well
				# above KEYSPACE most of even a genuinely cold
				# rep is warm, so this column measures the
				# warm-up TRANSIENT, not the cold cost.
				say "  (cold arm: each rep re-reads the ${KEYSPACE}-key"
				say "  keyspace several times over, so only the first"
				say "  pass is cold - this column is a warm-up"
				say "  TRANSIENT, not the cold cost.  Set N near"
				say "  KEYSPACE to weight it toward cold reads.)"
			fi
		fi
		$RT rm -f cb-n1 cb-n2 cb-n3 >/dev/null 2>&1
	# huge pages are not returned the instant a container dies, so the
	# next mode can find the pool still held and fail for a reason that
	# has nothing to do with it.  Wait for them back.
	sleep 3
	done
fi

say ""
say "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"

SUMDIR=$(dirname "$0")
[ -x "$SUMDIR/summarize.sh" ] && sh "$SUMDIR/summarize.sh" "$D/results.tsv"
