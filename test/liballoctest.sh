#!/bin/sh
# liballoctest.sh - S284: libperfd allocates nothing per call on a warm
# handle.
#
# An application links libperfd and brings its own allocator - on Alpine
# that is musl's, which serialises every malloc across threads (a
# musl-built client peaked at two threads, 20.6x behind glibc at eight,
# 2026-08).  perfcached cannot swap the host's allocator, so the library
# must not allocate per call.  libperfd 0.3.0 did, on every call: a
# malloc'd copy of every reply (ping included), the hash request's
# encoding, a heap writer for the JSON calls' requests and the reply
# reader's node array - 1 to 4 per call, counted with this suite's
# driver.  The reply is now borrowed from the handle until its next call,
# the requests are built in the handle's scratch and the reader's node
# array is lent by the handle.
#
# test/liballoc (libperfd, allocshim.so preloaded into it) runs 2,000 of
# each call on a warm handle and reports allocations per call:
#   0. positive control: the shim counts the driver's own malloc
#   1. every call: 0 - except perfd_get, perfd_jget and perfd_hget, whose
#      result is malloc'd for the caller by contract: exactly 1.  A call
#      that failed is a failure here, not a zero.
# Fail-first: libperfd 0.3.0 (the 0.4.6 release) allocates 1-4 per call.
# Usage: test/liballoctest.sh [./perfcached] [./liballoc] [./allocshim.so]
set -u
BIN=${1:-./perfcached}
DRV=${2:-./liballoc}
SHIM=${3:-./allocshim.so}
for f in "$DRV" "$SHIM"; do
	[ -f "$f" ] || { echo "liballoctest: $f is missing (make liballoc allocshim.so)"; exit 1; }
done
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
if ldd "$DRV" 2>/dev/null | grep -qE 'libasan|libclang_rt\.asan'; then
	echo "  SKIP a sanitizer runtime answers malloc itself - nothing to count"
	echo "liballoctest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
D=$(mktemp -d /var/tmp/pclal.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
PORT=17708
if ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]"; then
	echo "liballoctest: port $PORT already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = la-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "liballoctest: the node did not start: $(tail -3 "$D/n.log")"; exit 1; }
: > "$D/count"
PC_ALLOC_COUNT="$D/count" LD_PRELOAD="$SHIM" "$DRV" "$PORT" "$D/count" > "$D/run.out" 2>&1
RC=$?
CTL=$(sed -n 's/^control //p' "$D/run.out")
if [ "${CTL:-0}" = 0 ]; then
	echo "  SKIP the shim did not count the driver's own malloc - it is not in effect here (not glibc?)"
	echo "liballoctest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
echo "  ok   0. the shim is in effect: it counted the driver's own malloc"
pass=$((pass + 1))
[ $RC -eq 0 ] && grep -qE '^perfd_rl_hit ' "$D/run.out" || { echo "  FAIL the driver did not finish (rc $RC): $(tail -3 "$D/run.out")"; echo "liballoctest: $pass passed, 1 failed"; exit 1; }
while read -r call per want bad; do
	case "$call" in perfd_*) ;; *) continue;; esac
	if [ "${bad:-1}" != 0 ]; then
		echo "  FAIL 1. $call: $bad of 2,100 calls failed - a zero would mean nothing"
		fail=$((fail + 1))
	elif python3 -c "import sys; sys.exit(0 if abs(float('$per') - $want) < 0.01 else 1)"; then
		echo "  ok   1. $call: $per allocation(s) per call (want $want)"
		pass=$((pass + 1))
	else
		echo "  FAIL 1. $call: $per allocation(s) per call (want $want)"
		fail=$((fail + 1))
	fi
done < "$D/run.out"
echo "liballoctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
