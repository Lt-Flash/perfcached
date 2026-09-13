#!/bin/sh
# synctest.sh - S79: the MIT export boundary is ENFORCED, not remembered.
#  1. the real tree exports; every exported file is MIT; nothing exported
#     includes anything outside the export set (pc_noise.h once did).
#  2. a doctored tree in which one exported file reaches across the
#     boundary must REFUSE the export, before writing a single file.
# Usage: test/synctest.sh   (the perfcached argument, if given, is unused)
set -u
D=$(mktemp -d /var/tmp/pcsync.XXXXXX)
trap 'rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }
FILES="lib/perfd.c lib/perfd.h src/json.c src/json.h src/pc_noise.c src/pc_noise.h src/pc_slot.h src/pc_mix.h"

# 1. the real tree
if sh tools/sync-libperfd.sh "$D/out" >"$D/real.log" 2>&1; then ok; else bad "the real tree failed to export: $(cat "$D/real.log")"; fi
for f in $FILES; do
	[ -f "$D/out/$f" ] && grep -q "SPDX-License-Identifier: MIT" "$D/out/$f" && ok || bad "$f exported without an MIT SPDX line"
done
grep -q '#include "pc_attr.h"' "$D/out/src/pc_noise.h" && bad "pc_noise.h still includes the GPL pc_attr.h" || ok

# 2. a doctored tree: perfd.c reaches across
mkdir -p "$D/src/lib" "$D/src/src/compat"
cp lib/perfd.c lib/perfd.h "$D/src/lib/"
cp src/json.c src/json.h src/pc_noise.c src/pc_noise.h src/pc_slot.h src/pc_mix.h "$D/src/src/"
printf '#include "../src/pc_attr.h"\n' >> "$D/src/lib/perfd.c"
if SYNC_SRC="$D/src" sh tools/sync-libperfd.sh "$D/out2" >"$D/refuse.log" 2>&1; then
	bad "an include outside the export set crossed unrefused"
else
	grep -q "licence crossing" "$D/refuse.log" && ok || bad "refused for the wrong reason: $(cat "$D/refuse.log")"
fi
[ ! -e "$D/out2/lib/perfd.c" ] && ok || bad "a refused export still wrote files"

# 3. a file without the MIT line is refused too
mkdir -p "$D/src2"; cp -r "$D/src/." "$D/src2/"; sed -i '$d' "$D/src2/lib/perfd.c"   # drop the injected include
sed -i 's/SPDX-License-Identifier: MIT/SPDX-License-Identifier: GPL-2.0-or-later/' "$D/src2/src/pc_mix.h"
SYNC_SRC="$D/src2" sh tools/sync-libperfd.sh "$D/out3" >"$D/refuse2.log" 2>&1 \
	&& bad "a non-MIT file was exported" \
	|| { grep -q "no MIT SPDX" "$D/refuse2.log" && ok || bad "refused for the wrong reason: $(cat "$D/refuse2.log")"; }

# S84: the licence and the third-party notice travel with the code
[ -n "$(ls "$D"/out*/lib/NOTICE 2>/dev/null)" ] && [ -n "$(ls "$D"/out*/lib/LICENSE 2>/dev/null)" ] \
	&& ok "the export carries lib/LICENSE and lib/NOTICE" || bad "S84: LICENSE/NOTICE missing from the export: $(ls "$D"/out*/lib 2>/dev/null | tr '\n' ' ')"
echo "synctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
