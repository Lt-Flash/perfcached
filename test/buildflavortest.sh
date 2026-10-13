#!/bin/sh
# buildflavortest.sh - S357: make menuconfig chooses a build flavor (release |
# debug | asan) and the binary says which one it is.
#  1. menuconfig writes each flavor to config.mk and make reads it back
#     (make print-build); PC_BUILD on the command line overrides config.mk;
#     SAN set by hand makes the build an asan one; a flavor make or menuconfig
#     does not know is refused
#  2. the release flavor's flags are today's: -O2 -g, no -Og, no sanitizer,
#     no arena debug - no surprise optimisation change in what ships
#  3. -V and /stats process.build: the release binary under test says
#     nothing extra and "release"; a debug build of this tree says "debug"
#  4. the asan flavor's flags reach the link with the runtimes static: a
#     program with a use after free built with them aborts with an
#     AddressSanitizer report and needs no libasan (skipped on musl, where
#     the sanitizers do not run)
# Works on a copy of the tree, so the checkout's own config.mk is untouched.
# Usage: test/buildflavortest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
CC=${CC:-cc}
D=$(mktemp -d /var/tmp/pcflav.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkdir -p "$D/t"
tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' --exclude=config.mk . 2>/dev/null |
	(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
T="$D/t"
# the copy's makes run clean of the make that runs this suite: under `make
# check-asan` the outer make hands SAN=... to its recipes TWICE - in MAKEFLAGS
# (with -w) and, as GNU make exports a command-line variable, in the
# environment - and either one turns every flavor here into asan.  So both
# go, with the other build variables an outer make may pass: config.mk and
# this suite's own command lines decide here, nothing inherited.
M() { env -u MAKEFLAGS -u MAKELEVEL -u MAKEOVERRIDES -u MFLAGS \
	-u SAN -u CFLAGS -u LDLIBS -u PC_BUILD -u PC_EDITION -u PC_ALLOC -u PC_MAIL \
	make --no-print-directory "$@"; }
pb() { M -s -C "$T" print-build "$@" 2>&1; }

# ---- 1. config.mk round trip -------------------------------------------------
for f in release debug asan; do
	if [ $f = asan ] && "$CC" -dumpmachine 2>/dev/null | grep -q musl; then continue; fi
	(cd "$T" && sh tools/menuconfig.sh clustered libc $f > /dev/null 2>&1)
	grep -qx "PC_BUILD = $f" "$T/config.mk" && [ "$(pb | cut -d'|' -f1)" = $f ] &&
		ok "1. menuconfig writes PC_BUILD = $f and make reads $f back" ||
		bad "1. $f: config.mk '$(grep PC_BUILD "$T/config.mk")', make says '$(pb | cut -d'|' -f1)'"
done
(cd "$T" && sh tools/menuconfig.sh clustered libc release > /dev/null 2>&1)
[ "$(pb PC_BUILD=debug | cut -d'|' -f1)" = debug ] &&
	ok "1. PC_BUILD=debug on the command line overrides config.mk's release" ||
	bad "1. the command line did not override: $(pb PC_BUILD=debug | cut -d'|' -f1)"
[ "$(pb SAN=-fsanitize=address | cut -d'|' -f1)" = asan ] &&
	ok "1. SAN set by hand (make check-asan) makes the build say asan" ||
	bad "1. SAN by hand: $(pb SAN=-fsanitize=address | cut -d'|' -f1)"
pb PC_BUILD=fast | grep -q "PC_BUILD must be release, debug or asan" &&
	ok "1. make refuses PC_BUILD=fast, naming the three" || bad "1. PC_BUILD=fast: $(pb PC_BUILD=fast | tail -1)"
(cd "$T" && sh tools/menuconfig.sh clustered libc fast 2>&1) | grep -q "want release, debug or asan" &&
	grep -qx "PC_BUILD = release" "$T/config.mk" &&
	ok "1. menuconfig refuses 'fast' and leaves config.mk as it was" || bad "1. menuconfig took 'fast'"

# ---- 2. the release flags ----------------------------------------------------
R=$(pb | cut -d'|' -f2)
case " $R " in *" -O2 -g "*) ok "2. release: -O2 -g";; *) bad "2. release flags lost -O2 -g: $R";; esac
echo "$R" | grep -qE -- '-Og|-fsanitize|PCACHE_ARENA_DEBUG|-g3' &&
	bad "2. release carries debug or sanitizer flags: $R" || ok "2. release: no -Og, -g3, sanitizer or arena debug"
D2=$(pb PC_BUILD=debug | cut -d'|' -f2)
echo "$D2" | grep -q -- '-Og -g3 -fno-omit-frame-pointer' && echo "$D2" | grep -q PCACHE_ARENA_DEBUG &&
	ok "2. debug: -Og -g3 -fno-omit-frame-pointer -DPCACHE_ARENA_DEBUG" || bad "2. debug flags: $D2"

# ---- 3. the binary says what it is -------------------------------------------
# what the binary under test IS, read from the binary: the sanitizer
# ladder (make check-asan) hands this suite an asan build
EXP=release
grep -q __asan_init "$BIN" && EXP=asan
V=$("$BIN" -V | head -1)
case "$EXP:$V" in
release:*", debug)"*|release:*", asan)"*) bad "3. a release binary names a flavor: $V";;
release:*) ok "3. a release binary's -V is unchanged: $V";;
asan:*", asan)"*) ok "3. the binary under test is an asan build and -V says so: $V";;
*) bad "3. an asan binary's -V does not say so: $V";; esac
build_of() { # build_of <binary> -> process.build from a short start
	P1=$((17760 + $2)); P2=$((P1 + 1))
	printf '[daemon]\nworkers = 1\n[memory]\narena_mb = 32\n[secrets]\nclient = flav-secret\n[listen]\ntcp = 127.0.0.1:%d\nhttp = 127.0.0.1:%d\nplaintext = loopback\n[collection 0]\nbuckets_log2 = 8\n' \
		$P1 $P2 > "$D/n$2.conf"
	chmod 600 "$D/n$2.conf"
	ASAN_OPTIONS=detect_leaks=0 "$1" -f "$D/n$2.conf" > "$D/n$2.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n$2.log"; do sleep 0.1; i=$((i+1)); done
	python3 -c "import json,urllib.request; print(json.load(urllib.request.urlopen('http://127.0.0.1:$P2/stats',timeout=3))['process'].get('build'))" 2>/dev/null
	kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
}
B=$(build_of "$BIN" 0)
[ "$B" = "$EXP" ] && ok "3. /stats process.build of the binary under test: $EXP" || bad "3. process.build '$B' (want $EXP)"
(cd "$T" && sh tools/menuconfig.sh clustered libc debug > /dev/null 2>&1)
if M -s -C "$T" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.debug.log" 2>&1; then
	V=$("$T/perfcached" -V | head -1)
	case "$V" in *", debug)"*) ok "3. a debug build's -V names it: $V";; *) bad "3. debug -V: $V";; esac
	B=$(build_of "$T/perfcached" 2)
	[ "$B" = debug ] && ok "3. /stats process.build of the debug build: debug" || bad "3. debug process.build '$B'"
else
	bad "3. the debug flavor did not build: $(grep -m3 -E 'error|Error' "$D/build.debug.log" | tr '\n' ' ')"
fi

# ---- 4. the asan flags reach the link, runtimes static -----------------------
if "$CC" -dumpmachine 2>/dev/null | grep -q musl; then
	echo "  SKIP 4. musl - the sanitizers do not run here (menuconfig refuses asan)"; skip=$((skip+1))
else
	S=$(pb PC_BUILD=asan | cut -d'|' -f3)
	printf '#include <stdlib.h>\n#include <stdio.h>\nint main(void){volatile char *p=malloc(16);p[0]=1;free((void*)p);printf("%%d\\n",p[0]);return 0;}\n' > "$D/uaf.c"
	if "$CC" -O0 -g $S -o "$D/uaf" "$D/uaf.c" > "$D/uaf.build" 2>&1; then
		ASAN_OPTIONS=detect_leaks=0 "$D/uaf" > "$D/uaf.out" 2>&1
		grep -q "AddressSanitizer: heap-use-after-free" "$D/uaf.out" &&
			ok "4. a use after free built with the asan flavor's flags aborts with an AddressSanitizer report" ||
			bad "4. no AddressSanitizer report: $(head -3 "$D/uaf.out" | tr '\n' ' ')"
		ldd "$D/uaf" 2>/dev/null | grep -qE 'libasan|libubsan|libclang_rt' &&
			bad "4. the sanitizer runtimes are linked dynamically: $(ldd "$D/uaf" | grep -E 'asan|ubsan')" ||
			ok "4. the sanitizer runtimes are static: no libasan/libubsan to install on the target"
	else
		echo "  SKIP 4. this toolchain cannot link the static sanitizer runtimes: $(head -2 "$D/uaf.build" | tr '\n' ' ')"
		skip=$((skip+1))
	fi
fi

echo "buildflavortest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
