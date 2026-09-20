#!/bin/sh
# bench/run.sh — S10: single-node baselines, perfcached (text dialect,
# plaintext loopback) vs Redis, one client (pcbench) for both.
#
# CONTAINERISED 2026-09-12.  Nothing runs on the host: perfcached, redis
# 8 and pcbench are all containers, built from this tree's own image and
# docker.io/library/redis:8.  The host needs a container runtime and
# nothing else - no redis-server, no perfcached binary.
#
# Why the client SHARES the server's network namespace rather than
# talking across a bench network: this bench measures LOOPBACK, and it
# says so in its own title.  perfcached accepts the plaintext dialect
# only from loopback and pcbench has no handshake, so a bridge would
# either be refused by the daemon or silently measure a different path.
# `--network container:<server>` keeps 127.0.0.1 meaning what it meant
# before, with separate --cpuset-cpus on each side.
#
# Bench hygiene (the standing rules):
#   - every container from a previous run is removed first, by NAME -
#     never by pattern, which matches the shell running it;
#   - the image is built from this tree, and the revision is read back
#     OUT OF THE IMAGE, not from the host or the directory name;
#   - the build recipe is verified OPTIMIZED (-O2) before a number is
#     taken - it is the same Makefile the image build runs;
#   - every arm's server config is explicit below - nothing inherited;
#   - server pinned to cores 0-7, clients to 8-15 (16-vCPU host);
#   - each run refills+warms (2s discarded) - cold caches are not data;
#   - pcbench fails the arm loudly on any error reply.
#
# Usage: bench/run.sh [results-file]
set -eu

B=$(cd "$(dirname "$0")" && pwd)
. "$B/containerlib.sh"
cl_prog=run.sh

OUT=${1:-bench/results-$(date +%Y%m%d-%H%M).txt}
PPORT=16488
RPORT=6379                             # redis's own port, inside its container
KEYS=${BENCH_KEYS:-100000}
VAL=${BENCH_VAL:-64}
SECS=${BENCH_SECS:-10}
IMG=${IMG:-perfcached:bench}
CONTAINERFILE=${CONTAINERFILE:-bench/Containerfile.debian}
CPUSET_SRV=${CPUSET_SRV:-0-7}
CPUSET_CLI=${CPUSET_CLI:-8-15}
SRV=rn-srv                             # whichever server the arm is about
CLI=rn-cli
D=$(mktemp -d)

cleanup() { cl_rm "$CLI" "$SRV"; rm -rf "$D"; }
trap cleanup EXIT INT TERM

# ---- hygiene ---------------------------------------------------------------
cl_runtime_pick || exit 1
cleanup_prev() { cl_rm "$CLI" "$SRV"; }
cleanup_prev
# -nB: dry-run AS IF rebuilding, so the compile lines (and their -O2) are
# visible even when the binary is up to date.  This is the recipe the
# image build runs, so checking it here checks what will be measured.
make -nB perfcached | grep -q -- '-O2' || {
	echo "REFUSING: build recipe is not -O2"; exit 1; }
cl_image_build "$IMG" "$CONTAINERFILE" || exit 1

say() { echo "$@" | tee -a "$OUT"; }
RSV=$($RT run --rm --entrypoint redis-server "$CL_REDIS_IMG" --version |
	sed -n 's/.*v=\([0-9.]*\).*/\1/p')
say "== pcbench baselines, $(hostname), $(date -u +%FT%TZ) =="
say "== keys=$KEYS val=${VAL}B secs=$SECS warm=2s; server cores $CPUSET_SRV, client $CPUSET_CLI =="
say "== containers: $RT, image $IMG (perfcached $(cl_image_rev "$IMG")), redis $RSV =="

run() { # run <proto> <port> <conns> <depth> <getpct> <label>
	r=$(cl_exec "$CLI" pcbench -h 127.0.0.1 -p "$2" -P "$1" \
		-c "$3" -d "$4" -M "$5" -n $KEYS -v $VAL -T $SECS -w 2)
	say "$6: $r"
}

# the client always joins whichever server container is up, so 127.0.0.1
# in the line above is that server, every arm.
# NB: set CL_RUN_EXTRA on its own line and clear it after, never as a
# prefix assignment on the call.  POSIX leaves prefix assignments to a
# FUNCTION unspecified: dash, busybox and bash all scope them, but
# `bash --posix` leaves the value set, which would leak one container's
# --cpuset-cpus into the next one started - a silent mis-pin, and a
# mis-pinned arm produces a wrong number with no error anywhere.
client_up() {
	CL_RUN_EXTRA="--cpuset-cpus $CPUSET_CLI"
	cl_client_join "$CLI" "$SRV" "$IMG"
	_rc=$?; CL_RUN_EXTRA=""; return $_rc
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
	cl_rm "$CLI" "$SRV"
	# --ulimit memlock=-1: a container's default memlock sits far below
	# the arena, so mlock() fails and the arena runs UNPINNED and
	# swappable - a property of the container, not of the daemon.
	CL_RUN_EXTRA="--cpuset-cpus $CPUSET_SRV --ulimit memlock=-1"
	cl_node_up "$SRV" "" "" "$D/p.conf" "$IMG" || exit 1
	CL_RUN_EXTRA=""
	# the client comes up FIRST: readiness is read from the node's own
	# stats through perfcli, and perfcli runs in the client container
	client_up || exit 1
	cl_node_wait "$SRV" "$CLI" 127.0.0.1 "$PPORT" bench-client-secret || exit 1
}

stop_srv() { cl_rm "$CLI" "$SRV"; }

# ---- perfcached, 4 workers -------------------------------------------------
start_perf 4
say "-- perfcached workers=4 --"
run perf $PPORT 4 1  100 "GET rtt   "
run perf $PPORT 4 1  0   "SET rtt   "
run perf $PPORT 8 32 100 "GET piped "
run perf $PPORT 8 32 0   "SET piped "
run perf $PPORT 8 32 90  "MIX 90/10 "
stop_srv

# ---- perfcached, 8 workers (the scaling arm) -------------------------------
start_perf 8
say "-- perfcached workers=8 --"
run perf $PPORT 8 32 100 "GET piped "
run perf $PPORT 16 32 100 "GET piped c16"
stop_srv

# ---- redis -----------------------------------------------------------------
CL_RUN_EXTRA="--cpuset-cpus $CPUSET_SRV"
cl_redis_up "$SRV" "" || exit 1
CL_RUN_EXTRA=""
client_up || exit 1
say "-- redis (default single-threaded) --"
run resp $RPORT 4 1  100 "GET rtt   "
run resp $RPORT 4 1  0   "SET rtt   "
run resp $RPORT 8 32 100 "GET piped "
run resp $RPORT 8 32 0   "SET piped "
run resp $RPORT 8 32 90  "MIX 90/10 "
run resp $RPORT 16 32 100 "GET piped c16"
stop_srv

say "== done =="
echo "results in $OUT"
