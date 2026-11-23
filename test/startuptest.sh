#!/bin/sh
# startuptest.sh - start-up's failures and signals: a thread that cannot be created stops the
# start with a line naming it (never "ready" with the thread missing, never a crash at stop); a
# reload asked during start-up does not end the daemon.
set -u
BIN=${1:-./perfcached}
SHIM=$(readlink -f "${2:-./threadfailshim.so}")
[ -f "$SHIM" ] || { echo "startuptest: no $SHIM (make threadfailshim.so)"; exit 1; }
# a dynamic sanitizer runtime must come first in the preload list, the shim after it
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
D=$(mktemp -d /var/tmp/pcstartup.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19361 19362; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "startuptest: port $p busy" >&2; exit 1; }
done
cat > "$D/s.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = startup-client-secret
[listen]
tcp = 127.0.0.1:19361
resp = 127.0.0.1:19362
plaintext = loopback
[collection 0]
buckets_log2 = 10
C
chmod 600 "$D/s.conf"

# running and not a zombie (kill -0 succeeds on an exited child not yet waited for)
alive() { [ -r /proc/$1/stat ] && [ "$(cut -d' ' -f3 /proc/$1/stat 2>/dev/null)" != Z ]; }
# waits up to $1 tenths of a second for $P to exit; its status in $RC, empty if it did not
wait_exit() {
	n=0 RC=
	while [ $n -lt "$1" ]; do
		if ! alive $P; then
			wait $P; RC=$?; P=; return
		fi
		sleep 0.1; n=$((n+1))
	done
}

# A: the second worker, the maintenance thread, the WAL thread - each refused in turn
for c in "2 worker" "3 maintenance" "4 wal"; do
	nth=${c%% *} what=${c#* }
	: > "$D/log"
	LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_THREADFAIL_NTH=$nth "$BIN" -f "$D/s.conf" > "$D/log" 2>&1 &
	P=$!
	wait_exit 300
	if [ -z "$RC" ]; then
		bad "A: the $what thread not created - still running after 30 s ($(grep -c 'perfcached ready' "$D/log") ready line)"
		kill -9 $P 2>/dev/null; wait $P 2>/dev/null; P=
		continue
	fi
	if [ "$RC" -eq 1 ] && ! grep -q 'perfcached ready' "$D/log" &&
	        grep -q "could not start the $what thread" "$D/log"; then
		ok "A: the $what thread not created - start refused (exit 1), the line names it"
	else
		bad "A: the $what thread not created - exit $RC, $(grep -c 'perfcached ready' "$D/log") ready line, log: $(tail -2 "$D/log" | tr '\n' ' ')"
	fi
done

# B: a reload asked (SIGHUP) while start-up is still recovering - the daemon goes on to "ready"
# and stops clean (the default action of SIGHUP would have ended it)
: > "$D/log"
PERFCACHED_TEST_START_HOLD_MS=3000 "$BIN" -f "$D/s.conf" > "$D/log" 2>&1 &
P=$!
n=0
while [ $n -lt 300 ] && ! grep -q 'start-up holds' "$D/log"; do sleep 0.1; n=$((n+1)); done
kill -HUP $P 2>/dev/null
n=0
while [ $n -lt 300 ] && alive $P && ! grep -q 'perfcached ready' "$D/log"; do
	sleep 0.1; n=$((n+1))
done
if alive $P && grep -q 'perfcached ready' "$D/log"; then
	ok "B: SIGHUP during start-up - the daemon reached ready"
	kill $P 2>/dev/null
	wait_exit 300
	if [ "${RC:-x}" = 0 ] && grep -q 'clean shutdown' "$D/log"; then
		ok "B: and stopped clean on TERM"
	else
		bad "B: the stop after it - exit ${RC:-none}"
		[ -n "$P" ] && { kill -9 $P 2>/dev/null; wait $P 2>/dev/null; P=; }
	fi
else
	kill -9 $P 2>/dev/null
	wait $P 2>/dev/null; rc=$?; P=
	bad "B: SIGHUP during start-up ended the daemon (exit $rc), log: $(tail -1 "$D/log")"
fi

echo "startuptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
