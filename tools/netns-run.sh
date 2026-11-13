#!/bin/sh
# netns-run.sh - run a command in a private network namespace (lo up, a multicast route) so concurrent CI
# jobs on one host cannot collide on fixed ports or the group; without unshare it runs plainly.
if unshare -n true 2>/dev/null; then
	exec unshare -n sh -c \
		'ip link set lo up
		 ip route add 224.0.0.0/4 dev lo 2>/dev/null
		 exec "$@"' -- "$@"
fi
exec "$@"
