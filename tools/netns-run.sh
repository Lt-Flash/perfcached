#!/bin/sh
# netns-run.sh - run a command inside a private network namespace so
# concurrent CI jobs on ONE host cannot collide on the suites' fixed
# ports or multicast group (239.255.77.41).  The namespace needs its
# loopback up and a multicast route: the cluster plane rides 239/8 and
# every test endpoint lives in 127/8, so `lo` is the whole network.
# Where unshare is unavailable (docker executor without CAP_SYS_ADMIN,
# unprivileged runners) fall back to plain execution - those executors
# already isolate per job, which is why they never needed this.
if unshare -n true 2>/dev/null; then
	exec unshare -n sh -c \
		'ip link set lo up
		 ip route add 224.0.0.0/4 dev lo 2>/dev/null
		 exec "$@"' -- "$@"
fi
exec "$@"
