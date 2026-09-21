#!/bin/sh
# rpsrelay-node.sh <verb> [args] - one host's side of bench/rpsrelay.sh.
# Everything lives in $WORKDIR (default /tmp/rpsrelay); RUNTIME picks the
# container runtime for the Redis arm (podman, nerdctl or docker).
#
#   pc-start | pc-pid | pc-stop      perfcached from ./perfcached -f n.conf,
#                                    pinned to $SERVER_CPUS
#   stats | members                  its /stats and /members (http door)
#   redis-start | redis-stop         a cluster-enabled redis-server in a
#                                    container on the host network
#   redis-cli <args>                 redis-cli inside that container
#   cpu <pid>                        utime+stime ticks of a process
#   net <iface>                      tx bytes and packets of an interface
#   softirq                          softirq ticks summed over all CPUs
D=${WORKDIR:-/tmp/rpsrelay}
RT=${RUNTIME:-podman}
CPUS=${SERVER_CPUS:-0-7}
PORT_HTTP=${PORT_HTTP:-18991}
PORT_REDIS=${PORT_REDIS:-17993}
IMG=${REDIS_IMG:-docker.io/library/redis:8}
case "$1" in
pc-start)
	cd "$D" || exit 1
	rm -rf state && mkdir -p state && chmod 600 n.conf
	exec setsid -f taskset -c "$CPUS" ./perfcached -f n.conf > n.log 2>&1 < /dev/null ;;
pc-pid)
	for q in $(pgrep -x perfcached); do [ "$(readlink /proc/$q/cwd)" = "$D" ] && echo $q; done ;;
pc-stop)
	for q in $(pgrep -x perfcached); do [ "$(readlink /proc/$q/cwd)" = "$D" ] && kill $q; done
	sleep 2
	for q in $(pgrep -x perfcached); do [ "$(readlink /proc/$q/cwd)" = "$D" ] && kill -9 $q; done
	true ;;
stats)
	curl -s -m 5 "http://127.0.0.1:$PORT_HTTP/stats" ;;
members)
	curl -s -m 5 "http://127.0.0.1:$PORT_HTTP/members" ;;
redis-start)
	$RT rm -f rpsrelay-redis > /dev/null 2>&1
	$RT run -d --name rpsrelay-redis --network host --cpuset-cpus "$CPUS" "$IMG" \
		redis-server --port "$PORT_REDIS" --bind 0.0.0.0 --protected-mode no \
		--save '' --appendonly no --cluster-enabled yes \
		--cluster-config-file nodes.conf > /dev/null || exit 1
	$RT inspect -f '{{.State.Pid}}' rpsrelay-redis ;;
redis-cli)
	shift
	$RT exec rpsrelay-redis redis-cli -p "$PORT_REDIS" "$@" ;;
redis-stop)
	$RT rm -f rpsrelay-redis > /dev/null 2>&1
	true ;;
cpu)
	awk '{print $14+$15}' "/proc/$2/stat" ;;
net)
	awk -v i="$2:" '$1 == i {print $10, $11}' /proc/net/dev ;;
softirq)
	awk '/^cpu /{print $8}' /proc/stat ;;
*)
	echo "rpsrelay-node: unknown verb $1" >&2
	exit 2 ;;
esac
