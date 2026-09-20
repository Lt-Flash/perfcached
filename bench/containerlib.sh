# SPDX-License-Identifier: GPL-2.0-or-later
#
# containerlib.sh - the container plumbing every redis-using bench shares.
#
# Sourced, never executed:
#
#     . "$(dirname "$0")/containerlib.sh"
#     cl_prog=respbench
#     cl_runtime_pick || exit 1
#     cl_net_up pcrespnet 10.99.0.0/24
#     cl_redis_up rb-redis pcrespnet 10.99.0.10
#
# Why this exists: containerbench.sh grew a build-exercising runtime
# probe, a failure explainer that names causes, a network carrying the
# HOST's MTU, and a redis container - and every one of those was learned
# from a wrong number or a confusing failure (the comments name each).
# Four other harnesses need the same things.  Four copies drift, and the
# drift only shows up on a host that behaves differently from the one
# they were written on.
#
# containerbench.sh is deliberately NOT converted to source this.  It is
# the harness the README's published numbers come from, one run costs
# 5.5 hours, and breaking the working reference to remove a duplicate is
# a bad trade.  This code is derived from it; fold it back the next time
# containerbench is run end to end and can prove the change.
#
# Everything is prefixed cl_ so a harness's own names cannot collide.

CL_REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
cl_prog=${cl_prog:-bench}
RT=${RUNTIME:-}
CL_PROBE_ERR=""
CL_BUILD_ENV=""

# cl_do_build <runtime> <build args...> - ONE place that knows how to
# invoke a build, so the probe and the real build cannot diverge.
#
# Docker gets BUILDKIT FIRST.  The legacy builder starts each RUN step
# under the docker-default AppArmor profile, and on a host where that
# profile will not load every RUN dies with exit 185 - dockerd healthy,
# images pullable, builds impossible.  BuildKit is unaffected.  Legacy
# stays as the fallback for daemons too old for DOCKER_BUILDKIT=1.
cl_do_build() {
	_rt=$1; shift
	if [ "$_rt" = docker ]; then
		_o=$(DOCKER_BUILDKIT=1 docker build "$@" 2>&1) && {
			CL_BUILD_ENV="DOCKER_BUILDKIT=1"; printf '%s' "$_o"; return 0; }
		_o2=$(DOCKER_BUILDKIT=0 docker build "$@" 2>&1) && {
			CL_BUILD_ENV="DOCKER_BUILDKIT=0"; printf '%s' "$_o2"; return 0; }
		printf 'buildkit attempt:\n%s\nlegacy attempt:\n%s' "$_o" "$_o2"
		return 1
	fi
	_o=$("$_rt" build "$@" 2>&1) && { printf '%s' "$_o"; return 0; }
	printf '%s' "$_o"; return 1
}

# THE PROBE MUST RUN A BUILD STEP.  Two earlier versions were wrong:
# `FROM scratch` alone is rejected by Docker, so the probe failed on
# itself and blamed the runtime; `FROM scratch` + LABEL builds under
# both but starts NO container, so it passed on a host where AppArmor
# blocked every RUN and the real build died later.  A probe that does
# not exercise the path being probed turns "this will fail" into "this
# will fail later, with a more confusing message".
cl_probe_build() {
	_pd=$(mktemp -d) || return 1
	printf 'FROM docker.io/library/debian:13-slim\nRUN true\n' \
		> "$_pd/Containerfile"
	CL_PROBE_ERR=$(cl_do_build "$1" -f "$_pd/Containerfile" -t cl-probe:x "$_pd")
	_rc=$?
	"$1" rmi cl-probe:x >/dev/null 2>&1
	rm -rf "$_pd"
	return $_rc
}

# name the cause when the runtime's own message is unhelpful
cl_explain_build_failure() {
	case "$1" in
	*MAP_HUGETLB*|*hugetlb*|*huge*)
		echo "  -> a node exited just after taking huge pages.  On a host"
		echo "     with a RESERVED pool (nr_hugepages) the arenas come out"
		echo "     of that pool.  Check:  grep Huge /proc/meminfo"
		echo "     Then lower ARENA or raise the pool.  A killed node does"
		echo "     not return its pages instantly, so back-to-back arms can"
		echo "     fail for this reason when one arm alone succeeds." ;;
	*apparmor*|*AppArmor*)
		echo "  -> AppArmor refused to load the docker-default profile, so"
		echo "     no build container can start."
		if [ "$(systemd-detect-virt --container 2>/dev/null)" = lxc ] ||
		   grep -qa 'container=lxc' /proc/1/environ 2>/dev/null; then
			echo ""
			echo "     THIS IS AN LXC CONTAINER, and that is the cause."
			echo "     A guest cannot write profiles into the host's"
			echo "     AppArmor namespace, so nested Docker cannot start"
			echo "     containers at all.  On the LXC HOST:"
			echo "       LXD:      lxc config set <name> security.nesting true"
			echo "       Proxmox:  Options -> Features -> Nesting"
			echo "       lxc.conf: lxc.apparmor.profile = unconfined"
			echo "     then restart the guest.  Or use podman in the guest,"
			echo "     which loads no AppArmor profile: apt install podman"
		else
			echo "     Fixes, cheapest first:"
			echo "       systemctl restart apparmor docker"
			echo "       apt install --reinstall apparmor   (version skew)"
		fi ;;
	*buildkit*|*buildkitd*)
		echo "  -> nerdctl needs buildkitd: systemctl start buildkit" ;;
	*"Cannot connect"*|*"daemon"*)
		echo "  -> the daemon is not reachable: systemctl start docker" ;;
	esac
}

# cl_runtime_pick - honour $RUNTIME, else try podman, nerdctl, docker,
# keeping WHY each one failed.  "cannot build" with no reason sends
# people to install things they already have.
cl_runtime_pick() {
	if [ -n "$RT" ]; then
		command -v "$RT" >/dev/null 2>&1 || {
			echo "$cl_prog: RUNTIME=$RT is not in PATH" >&2; return 1; }
		cl_probe_build "$RT" && return 0
		echo "$cl_prog: RUNTIME=$RT cannot build images here." >&2
		echo "  it said:" >&2
		printf '%s\n' "$CL_PROBE_ERR" | sed 's/^/    /' | tail -6 >&2
		cl_explain_build_failure "$CL_PROBE_ERR" >&2
		return 1
	fi
	_tried=""
	for _c in podman nerdctl docker; do
		command -v $_c >/dev/null 2>&1 || continue
		if cl_probe_build $_c; then RT=$_c; return 0; fi
		_tried="$_tried
--- $_c said: ---
$(printf '%s' "$CL_PROBE_ERR" | tail -4)
$(cl_explain_build_failure "$CL_PROBE_ERR")"
		echo "$cl_prog: $_c is installed but cannot build - trying next" >&2
	done
	echo "$cl_prog: no container runtime here can build an image." >&2
	printf '%s\n' "$_tried" | sed 's/^/  /' >&2
	echo "" >&2
	echo "  Common causes:" >&2
	echo "    docker   - dockerd not running:  systemctl start docker" >&2
	echo "    nerdctl  - buildkitd not running: systemctl start buildkit" >&2
	echo "    podman   - usually works as-is;   apt install podman" >&2
	echo "" >&2
	echo "  Or name one explicitly: RUNTIME=docker $0" >&2
	return 1
}

# The MTU of the host's DEFAULT-ROUTE interface, not the runtime's 1500.
# Read throughput tracks MTU directly - a pipelined batch of 200 B GET
# responses is one segment at 65536, two at 9000, nine at 1500.  Measured
# on one host: GET 502,673/s at 1500, 793,905 at 9000.  A host whose NIC
# runs 9000 and whose bench ran at 1500 was measuring a network it does
# not have, and reporting its cache as slower than it is.
cl_host_mtu() {
	_hi=$(ip route show default 2>/dev/null | awk '{print $5; exit}')
	[ -n "$_hi" ] && ip link show "$_hi" 2>/dev/null |
		sed -n 's/.*mtu \([0-9]*\).*/\1/p' | head -1
}

# cl_net_up <net> <subnet> [mtu] - create the bench network.  Falls back
# to a network without the MTU option rather than failing: a wrong MTU
# is a measurement problem, no network at all is a dead run.  Verify with
# cl_net_mtu afterwards if the number matters.
cl_net_up() {
	_net=$1; _sub=$2; _mtu=${3:-$(cl_host_mtu)}
	case "$RT" in
	docker) _opt="com.docker.network.driver.mtu=$_mtu" ;;
	*)      _opt="mtu=$_mtu" ;;
	esac
	$RT network inspect "$_net" >/dev/null 2>&1 && return 0
	[ -n "$_mtu" ] &&
		$RT network create --subnet "$_sub" --opt "$_opt" "$_net" \
			>/dev/null 2>&1 && return 0
	$RT network create --subnet "$_sub" "$_net" >/dev/null 2>&1 && return 0
	echo "$cl_prog: could not create network $_net" >&2
	return 1
}

cl_net_down() { [ -n "$RT" ] || return 0; $RT network rm "$1" >/dev/null 2>&1; }

# cl_net_mtu <net> - the MTU a container on it ACTUALLY gets.  Read back
# from sysfs inside a container, not from the create's exit status: a
# host whose NIC runs 9000 was benchmarking across 1500 with no way to
# tell.  sysfs, not `ip link` - iproute2 is not in debian-slim, and
# moving to a Debian image once turned this into an empty string.
cl_net_mtu() {
	$RT run --rm --network "$1" --entrypoint sh "$CL_REDIS_IMG" \
		-c 'cat /sys/class/net/eth0/mtu' 2>/dev/null
}

# cl_redis_up <name> <net|""> [ip] - redis 8, persistence off.  An empty
# <net> means the runtime's default network: use it with a published port
# in $CL_RUN_EXTRA when a HOST process has to reach redis, which is the
# case whenever the subject under test is itself a host binary.  Other
# run arguments (pinning, memory) go in $CL_RUN_EXTRA too.
cl_redis_up() {
	_n=$1; _net=$2; _ip=${3:-}
	$RT rm -f "$_n" >/dev/null 2>&1
	# shellcheck disable=SC2086
	$RT run -d --name "$_n" ${_net:+--network "$_net"} \
		${_ip:+--ip "$_ip"} $CL_RUN_EXTRA \
		"$CL_REDIS_IMG" --save '' --appendonly no --protected-mode no \
		>/dev/null 2>&1 || {
		echo "$cl_prog: could not start redis container $_n" >&2; return 1; }
	if [ -n "$_net" ]; then
		cl_redis_wait "$_net" "${_ip:-$_n}"
	else
		cl_redis_wait_exec "$_n"
	fi
}

# cl_redis_wait_exec <name> [secs] - ping from INSIDE the container.  Use
# this when there is no bench network to ping across; it proves the
# server is up but says nothing about reachability, which is why the
# network form is preferred where a network exists.
cl_redis_wait_exec() {
	_n=$1; _lim=${2:-60}; _k=0
	while [ $_k -lt "$_lim" ]; do
		$RT exec "$_n" redis-cli PING 2>/dev/null | grep -q PONG && return 0
		sleep 1; _k=$((_k + 1))
	done
	echo "$cl_prog: redis container $_n did not answer PING in ${_lim}s" >&2
	return 1
}

# cl_redis_exec <name> <args...> - redis-cli inside the container.
cl_redis_exec() { _n=$1; shift; $RT exec "$_n" redis-cli "$@"; }

# cl_redis_cli <net> <host> <args...> - a throwaway redis-cli container.
cl_redis_cli() {
	_net=$1; _h=$2; shift 2
	$RT run --rm --network "$_net" --entrypoint redis-cli \
		"$CL_REDIS_IMG" -h "$_h" "$@" 2>/dev/null
}

# cl_redis_wait <net> <host> [secs] - until it answers PING.
cl_redis_wait() {
	_net=$1; _h=$2; _lim=${3:-60}; _k=0
	while [ $_k -lt "$_lim" ]; do
		cl_redis_cli "$_net" "$_h" PING | grep -q PONG && return 0
		sleep 1; _k=$((_k + 1))
	done
	echo "$cl_prog: redis at $_h did not answer PING in ${_lim}s" >&2
	return 1
}

# cl_client_up <name> <net> - a PERSISTENT client container.  The redis
# image carries redis-benchmark, so the load generator needs no image of
# its own; `tail -f /dev/null` keeps it alive between cells so the
# per-cell cost is an exec, not a container start.
cl_client_up() {
	_n=$1; _net=$2; _img=${3:-$CL_REDIS_IMG}
	$RT inspect -f '{{.State.Running}}' "$_n" 2>/dev/null | grep -q true &&
		return 0
	$RT rm -f "$_n" >/dev/null 2>&1
	# shellcheck disable=SC2086
	$RT run -d --name "$_n" ${_net:+--network "$_net"} $CL_RUN_EXTRA \
		--entrypoint tail "$_img" -f /dev/null >/dev/null 2>&1 || {
		echo "$cl_prog: could not start the client container $_n" >&2
		return 1; }
}

# cl_bench <client> <redis-benchmark args...>
cl_bench() { _n=$1; shift; $RT exec "$_n" redis-benchmark "$@"; }

# cl_image_build <tag> <containerfile> [rev] - build the perfcached bench
# image from the repo root.  --platform is pinned explicitly: a poisoned
# multi-arch cache once produced an arm/v7 image with x86 binaries in it,
# and every gate passed because the label came from the host.
cl_image_build() {
	_t=$1; _f=$2
	_rev=${3:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}
	if _o=$(cl_do_build "$RT" --platform linux/amd64 -f "$_f" \
			--build-arg REV="$_rev" -t "$_t" .); then
		return 0
	fi
	echo "$cl_prog: could not build $_t from $_f" >&2
	printf '%s\n' "$_o" | tail -20 | sed 's/^/    /' >&2
	cl_explain_build_failure "$_o" >&2
	return 1
}

# cl_image_rev <tag> - the revision the BINARY reports, read out of the
# image itself.  Never take it from the host, the directory name or the
# script that built it: that is how an ARM binary in an x86_64 tarball
# passed every gate under qemu.
cl_image_rev() {
	$RT run --rm --entrypoint perfcached "$1" -V 2>/dev/null |
		sed -n 's/.*(\(.*\)).*/\1/p' | head -1
}

# cl_node_up <name> <net> <ip|""> <conffile> <img> - a perfcached node.
# The config is bind-mounted read-only at the path the image's CMD names.
cl_node_up() {
	_n=$1; _net=$2; _ip=$3; _cf=$4; _img=$5
	$RT rm -f "$_n" >/dev/null 2>&1
	# shellcheck disable=SC2086
	$RT run -d --name "$_n" ${_net:+--network "$_net"} ${_ip:+--ip "$_ip"} \
		$CL_RUN_EXTRA -v "$_cf:/etc/perfcached.conf:ro" "$_img" \
		>/dev/null 2>&1 || {
		echo "$cl_prog: could not start node container $_n" >&2; return 1; }
}

# cl_node_state <exec-container> <addr> <nport> <secret> - the node's own
# lifecycle state, out of its stats.
cl_node_state() {
	$RT exec "$1" perfcli -h "$2" -p "$3" -a "$4" \
		-j '{"method":"stats"}' 2>/dev/null |
		grep -o '"state":"[a-z]*"' | head -1 | cut -d'"' -f4
}

# cl_node_wait <name> <exec-container> <addr> <nport> <secret> [secs] -
# until the node reports its own state as ready.
#
# NOT the log, which is what the host version of these harnesses grepped:
# perfcached has no stdout log target (`log_level` is its only logging
# option) so it logs to syslog, and `podman logs` on a perfectly healthy
# node is ZERO BYTES.  A log-grep gate therefore never fires and every
# fleet start times out - measured on 224, 2026-09-12, against a node
# that was up and answering the whole time.
#
# And NOT a PING either, though a PING does answer: a RECOVERING node
# answers on the RESP door while its readiness gate is still refusing
# data commands, so PING-as-gate would start benching a node that is
# still filling.  The node's own `state` is the only signal that
# distinguishes the two, which is exactly what the gate exists to do.
cl_node_wait() {
	_n=$1; _x=$2; _a=$3; _p=$4; _s=$5; _lim=${6:-60}; _k=0; _st=
	while [ $_k -lt "$_lim" ]; do
		_st=$(cl_node_state "$_x" "$_a" "$_p" "$_s")
		[ "$_st" = ready ] && return 0
		$RT inspect -f '{{.State.Running}}' "$_n" 2>/dev/null |
			grep -q true || {
			echo "$cl_prog: node $_n exited before becoming ready" >&2
			$RT inspect -f '  exit code: {{.State.ExitCode}}' "$_n" >&2
			return 1; }
		sleep 1; _k=$((_k + 1))
	done
	echo "$cl_prog: node $_n never reported ready in ${_lim}s \
(last state: ${_st:-no answer})" >&2
	return 1
}

# cl_client_join <name> <target> <img> - a persistent client container
# sharing the TARGET's network namespace, so the client reaches the
# server over 127.0.0.1.  This is not a convenience: perfcached accepts
# the plaintext dialect only from loopback (`plaintext = loopback`) and
# pcbench has no handshake, so a loopback baseline cannot be driven
# across a bridge at all.  Sharing the namespace keeps the measurement
# the loopback measurement it claims to be, with nothing on the host.
# Note a shared namespace still allows a separate --cpuset-cpus.
cl_client_join() {
	_n=$1; _tgt=$2; _img=$3
	$RT rm -f "$_n" >/dev/null 2>&1
	# shellcheck disable=SC2086
	$RT run -d --name "$_n" --network "container:$_tgt" $CL_RUN_EXTRA \
		--entrypoint tail "$_img" -f /dev/null >/dev/null 2>&1 || {
		echo "$cl_prog: could not start client $_n beside $_tgt" >&2
		return 1; }
}

# cl_exec <container> <cmd...> - run one of the image's own binaries.
cl_exec() { _n=$1; shift; $RT exec "$_n" "$@"; }

# cl_rm <name>... - remove containers, ignoring the ones never started.
# The empty-$RT guard is not defensive noise: these run from EXIT traps,
# and if the runtime probe failed then `$RT rm -f foo` degrades into the
# SHELL's `rm -f foo`, in whatever directory the harness happens to be.
cl_rm() { [ -n "$RT" ] || return 0; $RT rm -f "$@" >/dev/null 2>&1; return 0; }

CL_RUN_EXTRA=${CL_RUN_EXTRA:-}
