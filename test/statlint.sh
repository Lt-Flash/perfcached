#!/bin/sh
# statlint.sh — every counter the daemon INCREMENTS must be readable.
#
# WHY THIS EXISTS.  On 2026-09-13 two S127 counters were added to
# struct pc_proxy_stats and incremented on the hot paths, and neither
# was ever published in the stats dump.  They read as zero from outside
# for as long as they existed - which is worse than absent, because the
# instrumentation written to debug a live problem silently reported
# "nothing is happening" while the thing was happening.  Nothing caught
# it: the struct compiled, the increments compiled, and no test asks
# whether a counter can be READ.
#
# The rule: if the code does `px.foo++` or `C.px.foo++`, then "foo"
# must appear as a JSON key in the stats writer.  A counter that is
# deliberately internal has no business being incremented in the proxy
# stats struct - put it somewhere private instead.
#
# Usage: test/statlint.sh
set -u
cd "$(dirname "$0")/.." || exit 2
pass=0 fail=0
missing=

# every field incremented through the proxy-stats struct
NAMES=$(grep -hoE '\bC?\.?px\.[a-z_0-9]+\+\+' src/*.c 2>/dev/null \
	| sed 's/.*px\.//; s/++//' | sort -u)
[ -n "$NAMES" ] || { echo "statlint: found NO incremented px counters - the"
	echo "  pattern this lint greps for has changed, so it is asserting"
	echo "  nothing.  Fix the pattern before trusting a pass."; exit 1; }

for n in $NAMES; do
	if grep -q "\"$n\\\\\"" src/verbs.c 2>/dev/null || \
	   grep -q "\\\\\"$n\\\\\"" src/verbs.c 2>/dev/null; then
		pass=$((pass+1))
	else
		fail=$((fail+1)); missing="$missing $n"
	fi
done

echo "statlint: $pass readable, $fail NOT published"
if [ $fail -gt 0 ]; then
	echo "  these counters are incremented but appear in no stats dump:"
	for m in $missing; do echo "    px.$m"; done
	echo "  a counter that cannot be read is instrumentation that lies."
	exit 1
fi
exit 0
