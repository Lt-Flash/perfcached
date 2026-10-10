#!/bin/sh
# dirlocktest.sh - S290: one daemon per state_dir and per [wal] dir.
#
# Before S290 two daemons pointed at the same directories - different
# ports, a copy-pasted config - both started and both wrote the same
# identity, WAL and snapshot.  Now each directory is locked (fcntl, held
# for the process's life, released by the kernel when it dies).
#
#   1  a second daemon on the SAME directories is refused, naming the
#      directory and the holder's pid; the first keeps serving
#   2  a second daemon with its own state_dir but the SAME [wal] dir is
#      refused too - the WAL is the part that corrupts
#   3  kill -9 the holder: the next daemon on those directories starts -
#      nothing stale is left to clear
# Usage: test/dirlocktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcdl.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <name> <tcp> <http> <state_dir> <wal_dir>
	cat > "$D/$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $4
[memory]
arena_mb = 16
[secrets]
client = dirlock-client-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[wal]
dir = $5
probe = no
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/$1.conf"
}
ready() { # ready <log> -> 0 when the daemon said so (30 s: GitHub's
	# check-standalone on v0.4.1 gave up at 10 s with the daemon still in
	# its WAL probe and recovery on a slow-sync disk; the probe is off
	# now as well, as in the other WAL suites)
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$1" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
exited() { # exited <pid> <secs>
	k=0
	while [ $k -lt $(($2 * 10)) ]; do
		kill -0 $1 2>/dev/null || return 0
		sleep 0.1; k=$((k+1))
	done
	return 1
}
mkdir -p "$D/state" "$D/wal" "$D/state2"
conf a 17830 17831 "$D/state" "$D/wal"
conf b 17832 17833 "$D/state" "$D/wal"
conf c 17834 17835 "$D/state2" "$D/wal"

"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 & P1=$!
ready "$D/a.log" || { echo "daemon A did not start"; cat "$D/a.log"; exit 1; }

echo "--- 1: a second daemon on the same directories"
"$BIN" -f "$D/b.conf" > "$D/b.log" 2>&1 & P2=$!
if exited $P2 10; then
	wait $P2; rc=$?; P2=
	[ $rc -ne 0 ] && ok "it is refused (rc $rc)" || bad "it exited 0"
else
	bad "it is still running - two daemons share one state_dir"
	kill -9 $P2; P2=
fi
grep -q "state_dir $D/state is in use by another perfcached (pid $P1)" "$D/b.log" \
	&& ok "the message names the directory and the holder's pid" \
	|| { bad "no 'in use by another perfcached (pid $P1)' message"; tail -3 "$D/b.log"; }
python3 -c 'import json,sys,urllib.request; json.loads(urllib.request.urlopen("http://127.0.0.1:17831/stats",timeout=5).read())' 2>/dev/null \
	&& ok "the first daemon keeps serving" || bad "the first daemon stopped answering"
[ "$(cat "$D/state/perfcached.lock" 2>/dev/null)" = "$P1" ] && ok "the lock file carries the holder's pid" \
	|| bad "the lock file says '$(cat "$D/state/perfcached.lock" 2>/dev/null)', not $P1"

echo "--- 2: its own state_dir, the same [wal] dir"
"$BIN" -f "$D/c.conf" > "$D/c.log" 2>&1 & P2=$!
if exited $P2 10; then
	wait $P2; rc=$?; P2=
	[ $rc -ne 0 ] && ok "it is refused too (rc $rc)" || bad "it exited 0"
else
	bad "it is still running - two daemons share one WAL"
	kill -9 $P2; P2=
fi
grep -q "wal dir $D/wal is in use by another perfcached (pid $P1)" "$D/c.log" \
	&& ok "and the message names the WAL directory" \
	|| { bad "no WAL-directory message"; tail -3 "$D/c.log"; }

echo "--- 3: kill -9 the holder, start again on the same directories"
kill -9 $P1; wait $P1 2>/dev/null; P1=
"$BIN" -f "$D/b.conf" > "$D/b2.log" 2>&1 & P2=$!
ready "$D/b2.log" && ok "the next daemon starts - nothing stale to clear" \
	|| { bad "the next daemon did not start"; tail -3 "$D/b2.log"; }
kill $P2 2>/dev/null; wait $P2 2>/dev/null; P2=

echo "dirlocktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
