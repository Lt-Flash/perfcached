#!/bin/sh
# addrwaittest.sh - S294: a configured address that is not on the host
# YET is waited for, not failed on; one that never appears still fails.
#
# Each case runs the daemon in its own network namespace, so the address
# can be added after the start - the shape of an LXC whose network comes
# up after perfcached (245, 2026-10-01: the first bind failed
# EADDRNOTAVAIL, an ERROR, and systemd's restart picked it up 2 s later).
#
#   A  address added 3 s after the start -> waits, binds, ready, no ERROR
#   B  address never added, address_wait_s = 2 -> fails after the window
#   C  address_wait_s = 0 -> fails at once, says it did not wait
#
# Needs root (ip netns); SKIPs loudly without it.
# Usage: test/addrwaittest.sh [./perfcached]
set -u
BIN=$(readlink -f "${1:-./perfcached}")
ADDR=10.250.7.1
if [ "$(id -u)" != 0 ] || ! command -v ip >/dev/null 2>&1 ||
   ! ip netns add pcawprobe$$ 2>/dev/null; then
	echo "addrwaittest: SKIP - needs root and ip netns (nothing was tested)"
	exit 0
fi
ip netns del pcawprobe$$
D=$(mktemp -d /var/tmp/pcaw.XXXXXX)
NS=pcaw$$
P=
cleanup() { [ -n "$P" ] && kill -9 $P 2>/dev/null; ip netns del $NS 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <address_wait_s>
	cat > "$D/a.conf" <<EOF
[daemon]
workers = 1
log_level = notice
address_wait_s = $1
[memory]
arena_mb = 16
[secrets]
client = addrwait-client-secret
[listen]
tcp = $ADDR:17801
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/a.conf"
}
fresh_ns() {
	ip netns del $NS 2>/dev/null
	ip netns add $NS && ip -n $NS link set lo up
}
wait_exit() { # wait_exit <secs> -> 0 if $P exited within the window
	k=0
	while [ $k -lt $(($1 * 10)) ]; do
		kill -0 $P 2>/dev/null || return 0
		sleep 0.1; k=$((k+1))
	done
	return 1
}

echo "--- A: the address appears 3 s after the start"
fresh_ns; conf 30
ip netns exec $NS "$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
sleep 3
ip -n $NS addr add $ADDR/32 dev lo
i=0
while [ $i -lt 150 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	kill -0 $P 2>/dev/null || break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" && ok "the daemon came up once the address appeared" \
	|| { bad "the daemon did not come up"; tail -5 "$D/a.log"; }
grep -q "listener: waiting up to 30 s for $ADDR" "$D/a.log" && ok "it said it was waiting, and for what" \
	|| bad "no 'waiting up to 30 s' NOTICE"
grep -q "listener: $ADDR appeared after" "$D/a.log" && ok "it said when the address appeared" \
	|| bad "no 'appeared after' NOTICE"
if grep -q "ERROR" "$D/a.log"; then
	bad "the start logged an ERROR: $(grep ERROR "$D/a.log" | head -1)"
else
	ok "no ERROR in the start"
fi
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

echo "--- B: the address never appears, address_wait_s = 2"
fresh_ns; conf 2
ip netns exec $NS "$BIN" -f "$D/a.conf" > "$D/b.log" 2>&1 &
P=$!
if wait_exit 10; then
	wait $P; rc=$?
	[ $rc -ne 0 ] && ok "the start failed (rc $rc) after the window" || bad "the start exited 0"
else
	bad "the daemon was still running 10 s after a 2 s window"
fi
P=
grep -q "did not appear on this host within 2 s" "$D/b.log" && ok "the ERROR names the address and the window" \
	|| { bad "no 'did not appear within 2 s' ERROR"; tail -3 "$D/b.log"; }

echo "--- C: address_wait_s = 0"
fresh_ns; conf 0
ip netns exec $NS "$BIN" -f "$D/a.conf" > "$D/c.log" 2>&1 &
P=$!
if wait_exit 3; then
	wait $P; rc=$?
	[ $rc -ne 0 ] && ok "the start failed at once (rc $rc)" || bad "the start exited 0"
else
	bad "the daemon waited with address_wait_s = 0"
fi
P=
grep -q "address_wait_s = 0, not waiting" "$D/c.log" && ok "it said it did not wait" \
	|| bad "no 'not waiting' message"

echo "addrwaittest: $pass passed, $fail failed"
[ $fail -eq 0 ]
