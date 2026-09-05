#!/bin/sh
# sync-libperfd.sh — export the client library into a consumer tree
# (the OpenSIPS cachedb_perfd module, S28).
#
# Direction matters: perfcached vendors its ENGINE from OpenSIPS
# (tools/sync-core.sh), and OpenSIPS vendors its CLIENT from perfcached
# - each side owns what it authors, and libperfd stays the one client
# core.  The layout (lib/ + src/) is preserved so every relative
# include resolves untouched; only src/compat/dprint.h is NOT copied -
# the consumer supplies a shim mapping LM_* onto its own logger.
#
# LICENSING: every exported file is MIT (see lib/LICENSE) and carries
# its SPDX header; the export must never include src/core files, whose
# GPL-2.0-or-later hunks may carry OTHER PEOPLE'S copyright once
# upstream review changes flow back - the MIT boundary is exactly the
# file list below, and adding to it is a licensing decision, not a
# convenience.
#
# Usage: tools/sync-libperfd.sh <target-dir>
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SYNC_SRC:-$HERE}          # the tree to export (tests point it elsewhere)
DST=${1:?usage: sync-libperfd.sh <target-dir>}
REV=$(git -C "$HERE" rev-parse --short HEAD)

FILES="lib/perfd.c lib/perfd.h src/json.c src/json.h src/pc_noise.c src/pc_noise.h src/pc_slot.h src/pc_mix.h"
SHIMS="src/compat/dprint.h"      # supplied by the consumer, never copied

# The boundary is ENFORCED, not remembered (S79).  Before a byte is
# written: every exported file carries an MIT SPDX line, and every
# #include "..." in it resolves to another exported file or to the one
# shim the consumer supplies.  Anything else is a licence crossing and
# refuses the whole export - pc_noise.h once included pc_attr.h (GPL)
# for a compiler attribute, and a sync copied that crossing into a
# consumer tree before anyone noticed.
for f in $FILES; do
	[ -f "$SRC/$f" ] || { echo "sync-libperfd: $f missing from $SRC" >&2; exit 1; }
	grep -q "SPDX-License-Identifier: MIT" "$SRC/$f" \
		|| { echo "sync-libperfd: $f carries no MIT SPDX line - NOT exporting" >&2; exit 1; }
	d=$(dirname "$f")
	for inc in $(sed -n 's/^#include "\([^"]*\)".*/\1/p' "$SRC/$f"); do
		r=$(cd "$SRC/$d" && realpath -m --relative-to="$SRC" "$inc")
		case " $FILES $SHIMS " in
		*" $r "*) ;;
		*) echo "sync-libperfd: $f includes $r, which is outside the export set - a licence crossing; NOT exporting" >&2
		   exit 1 ;;
		esac
	done
done

mkdir -p "$DST/lib" "$DST/src/compat"
for f in $FILES; do
	{
		printf '/* VENDORED from Lt-Flash/perfcached %s (%s).\n' \
			"$REV" "$f"
		printf ' * Do not edit here - run tools/sync-libperfd.sh. */\n'
		cat "$SRC/$f"
	} > "$DST/$f"
done
echo "libperfd $REV -> $DST"
