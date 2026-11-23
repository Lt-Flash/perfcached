#!/bin/sh
# netns-run.sh - run a command in a private network namespace (lo up, a multicast route) so concurrent CI
# jobs on one host cannot collide on fixed ports or the group; without unshare it runs plainly.
# PC_NOSYNC=1: fsync and fdatasync return at once (libeatmydata), so the runners' shared-storage
# stalls stop timing the suites; a suite that measures real fsyncs clears LD_PRELOAD for its daemon,
# and one that preloads its own shims appends $PC_NOSYNC_LIB after them.
if [ "${PC_NOSYNC:-}" = 1 ]; then
	L=$(ls /usr/lib/*/libeatmydata.so* /usr/lib/libeatmydata.so* 2>/dev/null | head -1)
	if [ -n "$L" ]; then
		LD_PRELOAD="$L${LD_PRELOAD:+ $LD_PRELOAD}"; PC_NOSYNC_LIB=$L; export LD_PRELOAD PC_NOSYNC_LIB
		echo "netns-run: PC_NOSYNC - fsync is a no-op ($L)"
	else
		echo "netns-run: PC_NOSYNC set but libeatmydata is not installed - fsync is REAL"
	fi
fi
if unshare -n true 2>/dev/null; then
	exec unshare -n sh -c \
		'ip link set lo up
		 ip route add 224.0.0.0/4 dev lo 2>/dev/null
		 "$@"; rc=$?
		 # a process a suite left in this namespace would outlive the job: named, then killed
		 ns=$(readlink /proc/$$/ns/net)
		 for p in /proc/[0-9]*; do
			n=${p#/proc/}
			[ "$n" != $$ ] && [ "$(readlink "$p/ns/net" 2>/dev/null)" = "$ns" ] || continue
			c=$(tr "\0" " " < "$p/cmdline" 2>/dev/null | cut -c1-160)
			[ -n "$c" ] && kill -9 "$n" 2>/dev/null && echo "netns-run: killed $n, left running: $c"
		 done
		 exit $rc' -- "$@"
fi
exec "$@"
