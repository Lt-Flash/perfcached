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
DST=${1:?usage: sync-libperfd.sh <target-dir>}
REV=$(git -C "$HERE" rev-parse --short HEAD)

mkdir -p "$DST/lib" "$DST/src/compat"
for f in lib/perfd.c lib/perfd.h src/json.c src/json.h \
         src/pc_noise.c src/pc_noise.h src/pc_slot.h src/pc_mix.h; do
	{
		printf '/* VENDORED from Lt-Flash/perfcached %s (%s).\n' \
			"$REV" "$f"
		printf ' * Do not edit here - run tools/sync-libperfd.sh. */\n'
		cat "$HERE/$f"
	} > "$DST/$f"
done
echo "libperfd $REV -> $DST"
