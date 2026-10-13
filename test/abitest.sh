#!/bin/sh
# abitest.sh - S370: libperfd.so keeps the ABI its SONAME promises.
#   1  its SONAME is libperfd.so.<PERFD_SOVER>
#   2  it exports every symbol the frozen list for that SONAME names
#      (test/abi/libperfd.so.<N>.symbols) - one missing is an ABI break and
#      needs a new SONAME; one added is fine (and joins the list when frozen)
#   3  it exports nothing but perfd_* (the version script)
#   4  a caller built against the FROZEN header of that SONAME
#      (test/abi/perfd-abi<N>.h) runs against it: connect, ping, set, get,
#      perfd_free_value, an error's kind
# Fail-first: a version script that hides perfd_ping fails 2 and 4.
# Usage: test/abitest.sh [./perfcached] [./abicaller]
set -u
BIN=${1:-./perfcached}
CALLER=${2:-./abicaller}
SOVER=$(sed -n 's/^PERFD_SOVER := //p' Makefile)
VER=$(sed -n 's/^#define PERFD_VERSION "\(.*\)"/\1/p' lib/perfd.h)
SO=libperfd.so.$VER
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SO" ] || { echo "abitest: $SO is missing (make libperfd.so)"; exit 1; }
N=$(readelf -d "$SO" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
[ "$N" = "libperfd.so.$SOVER" ] && ok "1. SONAME $N" || bad "1. SONAME '$N' (want libperfd.so.$SOVER)"
nm -D --defined-only "$SO" | awk '{print $3}' | sort > /tmp/abitest.$$.syms
MISSING=$(comm -23 test/abi/libperfd.so.$SOVER.symbols /tmp/abitest.$$.syms | tr '\n' ' ')
ADDED=$(comm -13 test/abi/libperfd.so.$SOVER.symbols /tmp/abitest.$$.syms | grep '^perfd_' | tr '\n' ' ')
[ -z "$MISSING" ] && ok "2. every symbol of libperfd.so.$SOVER is still exported ($(wc -l < test/abi/libperfd.so.$SOVER.symbols))${ADDED:+ - added: $ADDED}" \
	|| bad "2. an ABI break without a new SONAME - missing: $MISSING"
OTHER=$(grep -v '^perfd_' /tmp/abitest.$$.syms | tr '\n' ' ')
[ -z "$OTHER" ] && ok "3. nothing but perfd_* is exported" || bad "3. exported beyond the API: $OTHER"
rm -f /tmp/abitest.$$.syms
D=$(mktemp -d /var/tmp/pcabi.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
printf '[daemon]\nworkers = 1\n[memory]\narena_mb = 16\n[secrets]\nclient = abitest-secret\n[listen]\ntcp = 127.0.0.1:18591\nplaintext = loopback\n[collection c]\nbuckets_log2 = 8\n' > "$D/c.conf"
chmod 600 "$D/c.conf"
"$BIN" -f "$D/c.conf" > "$D/log" 2>&1 & P=$!
i=0; while [ $i -lt 50 ] && ! grep -q "perfcached ready" "$D/log"; do sleep 0.1; i=$((i+1)); done
R=$("$CALLER" 18591 2>&1); rc=$?
[ $rc = 0 ] && ok "4. a caller built against the frozen header runs against it ($R)" \
	|| bad "4. the frozen-header caller: rc $rc: $(echo "$R" | tr '\n' ' ')"
echo "abitest: $pass passed, $fail failed"
[ $fail -eq 0 ]
