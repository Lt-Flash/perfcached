#!/bin/sh
# readmetest.sh - the README's status names the version being built, and
# the CHANGELOG has a section for it.  A status that says "release
# candidate 16" while version.h says rc17 is the drift a reader punishes
# with distrust; this makes it a red build instead (check-fast runs on
# every push and gates a tag).  The README wraps at 72 columns, so the
# status sentence is matched with its line breaks folded.
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
	if printf '%s' "$FOLDED" | grep -qE "last tagged release is \*\*$V\*\*"; then
		ok "README status: the last tagged release is $V (version.h says $V)"
	else
		bad "README status does not say the last tagged release is $V (version.h says $V)"
	fi
	;;
esac
if grep -qE "^## $V " CHANGELOG.md; then
	ok "CHANGELOG has a '## $V' section"
else
	bad "CHANGELOG has no '## $V' section"
fi
echo "readmetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
