#!/bin/sh
# readmetest.sh - the README's status, the ChangeLog banner, the APKBUILD and libperfd's version all
# agree with version.h (the README's 72-column wrap folded).
set -u
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
V=$(sed -n 's/^#define PC_VERSION "\(.*\)"/\1/p' src/version.h)
[ -n "$V" ] || { bad "src/version.h has no PC_VERSION"; echo "readmetest: 0 passed, 1 failed"; exit 1; }
LINE=${V%%-*}                         # 0.3.7 of 0.3.7-rc16
FOLDED=$(tr -s '[:space:]' ' ' <README.md)
case $V in
*-rc*)
	RC=${V##*-rc}
	if printf '%s' "$FOLDED" | grep -qE "the \*\*$LINE\*\* line is at release candidate $RC\b"; then
		ok "README status: the $LINE line is at release candidate $RC (version.h says $V)"
	else
		bad "README status does not say the $LINE line is at release candidate $RC (version.h says $V)"
	fi
	;;
*)
	if printf '%s' "$FOLDED" | grep -qE "\*\*Current release: $V\*\*"; then
		ok "README status: the current release is $V (version.h says $V)"
	else
		bad "README status does not say **Current release: $V** (version.h says $V)"
	fi
	;;
esac
if grep -qE "^=+ Release $V =+$" ChangeLog; then
	ok "ChangeLog has a 'Release $V' banner"
else
	bad "ChangeLog has no 'Release $V' banner"
fi
# the Alpine APKBUILD's pkgver must follow version.h: alpine-pkg-check names the tarball after it
P=$(sed -n 's/^pkgver=//p' contrib/alpine/APKBUILD 2>/dev/null)
if [ "$P" = "$V" ]; then
	ok "contrib/alpine/APKBUILD pkgver is $V"
else
	bad "contrib/alpine/APKBUILD pkgver is '$P', version.h says $V"
fi
# libperfd's version (the dashboard's -V reports it): a client-only fix may take it ahead of the
# daemon's first three numbers, never behind; each carries its own fourth number
L=$(sed -n 's/^#define PERFD_VERSION "\(.*\)"/\1/p' lib/perfd.h 2>/dev/null)
B=$(printf '%s' "$LINE" | cut -d. -f1-3)
if [ -n "$L" ] && [ "$(printf '%s\n%s\n' "$B" "$L" | sort -V | head -n 1)" = "$B" ]; then
	ok "lib/perfd.h PERFD_VERSION is $L, not behind $B (version.h says $V)"
else
	bad "lib/perfd.h PERFD_VERSION is '$L', version.h says $V - libperfd must be $B or later (the dashboard's -V reports libperfd's)"
fi
# the numeric defines build the version fact /members shows: they must spell PC_VERSION, the fourth
# number when it is not 0
vn() { sed -n "s/^#define PC_VERSION_$1 \([0-9]*\).*/\1/p" src/version.h; }
N="$(vn MAJOR).$(vn MINOR).$(vn PATCH)"
[ "$(vn TWEAK)" = 0 ] || N="$N.$(vn TWEAK)"
if [ "$N" = "$LINE" ]; then
	ok "version.h's numeric defines spell $N"
else
	bad "version.h's numeric defines spell $N, PC_VERSION says $V"
fi
# and libperfd's numeric defines spell PERFD_VERSION, the fourth number when it is not 0
lv() { sed -n "s/^#define PERFD_VERSION_$1 \([0-9]*\).*/\1/p" lib/perfd.h; }
NL="$(lv MAJOR).$(lv MINOR).$(lv PATCH)"
[ "$(lv TWEAK)" = "" ] || [ "$(lv TWEAK)" = 0 ] || NL="$NL.$(lv TWEAK)"
if [ "$NL" = "$L" ]; then
	ok "lib/perfd.h's numeric defines spell $NL"
else
	bad "lib/perfd.h's numeric defines spell $NL, PERFD_VERSION says $L"
fi
echo "readmetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
