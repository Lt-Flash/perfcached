#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# mitboundtest.sh - libperfd's MIT boundary as the compiler reads it: every file the library is
# built from carries an MIT SPDX line, the archive holds only PERFD_SRCS, and a doctored tree fails.
set -u
AR=${1:?usage: mitboundtest.sh <libperfd.a>}
CC=${CC:-cc}
D=$(mktemp -d /var/tmp/mitbound.XXXXXX)
trap 'rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

PL=$(make -s print-libperfd) || { echo "make print-libperfd failed"; exit 2; }
SRCS=${PL%%|*}
FLAGS=${PL#*|}

# the files the compiler reads under <root> to build the library, one per line
deps() {
	(cd "$1" && $CC $FLAGS -MM $SRCS) > "$D/mm" || return 1
	for f in $(tr ' \\' '\n\n' < "$D/mm" | grep -v ':$' | grep .); do
		(cd "$1" && realpath -m --relative-to=. "$f")
	done | sort -u
}
# the ones among them without an MIT SPDX line
crossings() {
	deps "$1" > "$D/dl" || { echo "(the compiler could not resolve the tree)"; return; }
	for f in $(cat "$D/dl"); do
		grep -q "SPDX-License-Identifier: MIT" "$1/$f" || echo "$f"
	done
}
# a copy of the library's side of the tree to doctor
doctor_copy() {
	mkdir -p "$1/src"
	cp -r lib "$1/"
	cp src/*.c src/*.h "$1/src/"
}

# 1. the real tree
deps . > "$D/deps" || { echo "FAIL: the compiler could not list the library's sources"; exit 1; }
n=$(wc -l < "$D/deps")
# positive control: the set is the library, not an empty list
for f in lib/perfd.c lib/perfd-connect.inc lib/shim/compat/dprint.h src/pc_noise.h src/ptree.h; do
	grep -qx "$f" "$D/deps" && ok || bad "$f is not among the $n files the library compiles from"
done
x=$(crossings .)
[ -z "$x" ] && ok || bad "the library compiles from non-MIT file(s): $x"
grep -qx "src/compat/dprint.h" "$D/deps" && bad "the library reads the daemon's GPL dprint.h" || ok

# 2. the archive: PERFD_SRCS' objects and nothing else
want=$(for s in $SRCS; do basename "${s%.c}.o"; done | sort | tr '\n' ' ')
have=$(ar t "$AR" | sort | tr '\n' ' ')
[ "$want" = "$have" ] && ok || bad "$AR holds [$have], expected [$want]"

# 3. doctored trees: the check must be able to fail
doctor_copy "$D/t1"
printf '#include "../src/pc_attr.h"\n' >> "$D/t1/lib/perfd.c"
x=$(crossings "$D/t1")
[ "$x" = "src/pc_attr.h" ] && ok || bad "a GPL include from perfd.c was not caught (got: '$x')"

doctor_copy "$D/t2"
sed -i 's/SPDX-License-Identifier: MIT/SPDX-License-Identifier: GPL-2.0-or-later/' "$D/t2/src/pc_mix.h"
x=$(crossings "$D/t2")
[ "$x" = "src/pc_mix.h" ] && ok || bad "a relabelled header was not caught (got: '$x')"

echo "mitboundtest: $pass passed, $fail failed ($n files in the boundary)"
[ $fail -eq 0 ]
