#!/bin/sh
# statlint.sh - every counter incremented through the proxy-stats struct (px.foo++) must be readable:
# its key in a stats writer, or px.foo handed to a JSON writer call.  A zero that never moves lies.
set -u
cd "$(dirname "$0")/.." || exit 2
pass=0 fail=0
missing=
# where the stats tree is written - a file that is not there fails loudly,
# or a move would turn every check into "not published"
W="src/statstree.c src/statscluster.c src/verbs.c"
for f in $W; do
	[ -f "$f" ] || { echo "statlint: $f is gone - point W at the stats writer"; exit 1; }
done

# every field incremented through the proxy-stats struct
NAMES=$(grep -hoE '\bC?\.?px\.[a-z_0-9]+\+\+' src/*.c 2>/dev/null \
	| sed 's/.*px\.//; s/++//' | sort -u)
[ -n "$NAMES" ] || { echo "statlint: found NO incremented px counters - the"
	echo "  pattern this lint greps for has changed, so it is asserting"
	echo "  nothing.  Fix the pattern before trusting a pass."; exit 1; }

for n in $NAMES; do
	# a counter is published by its key, pc_tw_key(out, "name"), and its value, pc_tw_i64(out,
	# px.name); the escaped JSON-key forms stay for any writer left on the RESP side
	if grep -q "\"$n\\\\\"" $W 2>/dev/null || \
	   grep -q "\\\\\"$n\\\\\"" $W 2>/dev/null || \
	   grep -qE "pc_tw_key\\(out, *\"$n\"\\)" $W 2>/dev/null || \
	   grep -qE "pc_tw_ctr(_col)?\\(out, [^;]*\"$n\"," $W 2>/dev/null || \
	   grep -qE "pc_(jw|tw)_[a-z0-9]+\\(out, *(\\(long long\\))?px\\.$n\\);" $W 2>/dev/null; then
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
