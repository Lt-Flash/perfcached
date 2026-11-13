#!/bin/sh
# nightly-valgrind.sh [binary ...] - memcheck (uninitialised reads ASan misses) over the unit
# binaries only: the integration suites' fixed startup waits time out under valgrind's slowdown.
set -u
cd "$(dirname "$0")/.." || exit 1

VG_OPTS="--tool=memcheck --leak-check=full --show-leak-kinds=definite
	--track-origins=yes --errors-for-leak-kinds=definite
	--num-callers=25 --error-exitcode=99"
[ -f tools/valgrind.supp ] && VG_OPTS="$VG_OPTS --suppressions=tools/valgrind.supp"

BINS="${*:-clustersim slottest jwtest clmaptest cltermtest clsynctest
	clhisttest clplacetest clseltest memprobe noisetest wipetest}"
LOGD=$(mktemp -d /var/tmp/vgnight.XXXXXX)
trap 'rm -rf "$LOGD"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs

command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 1; }
make -j"$(nproc)" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
# the binary's stamp (PC_BUILD_REV) can be stale after an incremental build: print the
# tree's head beside it
echo "nightly valgrind: $(valgrind --version), tree $(git rev-parse --short HEAD 2>/dev/null || echo '?'), binary stamp $(./perfcached -V 2>/dev/null | head -1)"

bad=0 ran=0
for b in $BINS; do
	[ -x "./$b" ] || { echo "  -- $b: not built, skipped"; continue; }
	# clustersim takes an iteration count; keep the nightly bounded
	case "$b" in clustersim) ARGS=300 ;; *) ARGS= ;; esac
	ran=$((ran + 1))
	# shellcheck disable=SC2086
	valgrind $VG_OPTS --log-file="$LOGD/$b.log" "./$b" $ARGS \
		> "$LOGD/$b.out" 2>&1
	rc=$?
	errs=$(sed -n 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/p' \
		"$LOGD/$b.log" | tail -1)
	if [ "$rc" = 99 ] || [ "${errs:-0}" != 0 ]; then
		bad=$((bad + 1))
		echo "  FAIL $b: ${errs:-?} error(s)"
		grep -E "Invalid |uninitialised|Conditional jump|definitely lost" \
			"$LOGD/$b.log" | head -4 | sed 's/^/      /'
		grep -A6 "ERROR SUMMARY" "$LOGD/$b.log" | head -2 | sed 's/^/      /'
	elif [ "$rc" != 0 ]; then
		bad=$((bad + 1))
		echo "  FAIL $b: exited $rc under valgrind (clean memcheck)"
		tail -3 "$LOGD/$b.out" | sed 's/^/      /'
	else
		echo "  ok   $b"
	fi
done
echo "nightly valgrind: $((ran - bad))/$ran clean"
[ "$bad" -eq 0 ]
