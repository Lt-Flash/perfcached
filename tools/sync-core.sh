#!/bin/sh
# sync-core.sh — vendored-core provenance and drift tool.
#
# The htable/arena/mem core in src/core/ is vendored from the OpenSIPS
# cachedb_perf module tree, which remains the upstream truth until this
# project truly forks. Local modifications to vendored files ARE expected
# (compat-shim rewiring); this tool exists so upstream fixes are NOTICED,
# never auto-merged.
#
# usage:
#   tools/sync-core.sh pull     copy manifest files from upstream HEAD,
#                               stamping a provenance header (initial
#                               vendoring / deliberate re-sync)
#   tools/sync-core.sh status   per file: OK (upstream unchanged since the
#                               recorded commit) or DRIFT
#   tools/sync-core.sh diff     show the upstream delta (recorded..HEAD)
#                               for every drifted file
#
# environment:
#   PERFCACHED_UPSTREAM  upstream git worktree
#                        (default /dn/wt-pullshare-hg)
#   VENDOR_DIR           vendored-file directory (default src/core)

set -eu

UPSTREAM=${PERFCACHED_UPSTREAM:-/dn/wt-pullshare-hg}
SUBDIR=modules/cachedb_perf
VENDOR_DIR=${VENDOR_DIR:-src/core}
FILES="pcache_htable.c pcache_htable.h pcache_arena.c pcache_arena.h pcache_mem.c pcache_mem.h"

[ -d "$UPSTREAM/.git" ] || [ -f "$UPSTREAM/.git" ] || {
	echo "upstream worktree not found: $UPSTREAM (set PERFCACHED_UPSTREAM)" >&2
	exit 2
}

up_head() { git -C "$UPSTREAM" rev-parse HEAD; }
up_branch() { git -C "$UPSTREAM" branch --show-current; }

recorded_commit() { # $1 = vendored file
	sed -n 's/^ \* *commit: *\([0-9a-f]\{7,40\}\).*/\1/p' "$1" | head -1
}

do_pull() {
	head=$(up_head); branch=$(up_branch); day=$(date -u +%Y-%m-%d)
	mkdir -p "$VENDOR_DIR"
	for f in $FILES; do
		{
			printf '/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module\n'
			printf ' * upstream: %s\n' "$UPSTREAM"
			printf ' * branch: %s\n' "$branch"
			printf ' * commit: %s\n' "$head"
			printf ' * path: %s/%s   synced: %s\n' "$SUBDIR" "$f" "$day"
			printf ' * Local modifications ARE expected (compat-shim rewiring).\n'
			printf ' * Check upstream drift with tools/sync-core.sh status|diff. */\n'
		} > "$VENDOR_DIR/$f"
		git -C "$UPSTREAM" show "HEAD:$SUBDIR/$f" >> "$VENDOR_DIR/$f"
		echo "pulled $f @ ${head}"
	done
}

do_status() {
	rc=0
	for f in $FILES; do
		v="$VENDOR_DIR/$f"
		if [ ! -f "$v" ]; then
			echo "MISSING  $f (not vendored yet — run pull)"; rc=1; continue
		fi
		rec=$(recorded_commit "$v")
		if [ -z "$rec" ]; then
			echo "NOSTAMP  $f (no provenance header)"; rc=1; continue
		fi
		if git -C "$UPSTREAM" diff --quiet "$rec"..HEAD -- "$SUBDIR/$f"; then
			echo "OK       $f @ $rec"
		else
			echo "DRIFT    $f ($rec..$(up_head | cut -c1-10) touches it — see diff)"
			rc=1
		fi
	done
	return $rc
}

do_diff() {
	for f in $FILES; do
		v="$VENDOR_DIR/$f"
		[ -f "$v" ] || continue
		rec=$(recorded_commit "$v"); [ -n "$rec" ] || continue
		git -C "$UPSTREAM" diff --quiet "$rec"..HEAD -- "$SUBDIR/$f" && continue
		echo "===== upstream delta for $f ($rec..HEAD) ====="
		git -C "$UPSTREAM" diff "$rec"..HEAD -- "$SUBDIR/$f"
	done
}

case "${1:-}" in
	pull)   do_pull ;;
	status) do_status ;;
	diff)   do_diff ;;
	*) echo "usage: $0 pull|status|diff" >&2; exit 2 ;;
esac
