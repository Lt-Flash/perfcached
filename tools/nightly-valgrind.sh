#!/bin/sh
# nightly-valgrind.sh — memcheck over the UNIT binaries (S55 item 4).
#
# Scope, deliberately: the unit binaries, not `make check` wholesale.
# Valgrind slows execution roughly 20x, and the integration suites start
# a real daemon behind fixed startup waits - under memcheck those time
# out, so a full-suite run would report timeouts rather than findings and
# teach everyone to ignore a red nightly.  That is precisely the argument
# DESIGN 12ad used to defer TSan, and it applies here.
#
# The unit binaries are also where the class this job exists for lives:
# uninitialised reads, which ASan does not see and MSan cannot reach
# (every dependency, libsodium included, would need instrumenting).
#
# Usage: tools/nightly-valgrind.sh [binary ...]   (default: all of them)
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

command -v valgrind >/dev/null || { echo "valgrind not installed"; exit 1; }
make -j"$(nproc)" >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
# The binary's stamp comes from PC_BUILD_REV, which an incremental
# build leaves at whatever it was when that object last compiled - it
# has misreported the revision more than once.  Print the TREE's head
# beside it so a stale stamp is visible rather than believed.
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
