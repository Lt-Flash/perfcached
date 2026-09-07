#!/bin/sh
# clonetest.sh — a node whose identity file was copied with the machine.
#
# The identity is persisted, so cloning a VM or container clones it too,
# and two daemons then claim to BE each other.  Everything built on
# identity stops meaning anything: whether a node restarted, who should
# backfill it, whether a keyspace was already sent.  The master refuses
# such a join outright and the clone stops, rather than serving clients
# stale data under a name that belongs to someone else.
#
# Asserted:
#  - a genuine RESTART (same identity, same address) is still admitted;
#  - a CLONE (same identity, different address) is refused and exits;
#  - it says why, and names the fix;
#  - the original is undisturbed.
# usage: test/clonetest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcclone.XXXXXX)
SEC=cl-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT

for pf in 17961 17962 17963; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "clonetest: port $pf already bound" >&2; exit 1; }
done

mk() {
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = cl-cluster-secret
[listen]
tcp = 127.0.50.$1:1796$1
[cluster]
multicast = 239.255.77.157:17257
advertise = 127.0.50.$1
pull_timeout_ms = 400
mode = store
collections = c
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
fsync = no
save = off
[collection c]
buckets_log2 = 12
EOF
}
start() { # start <id> <tag>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
alive() { pgrep -f "[p]erfcached -f $D/n$1.conf" >/dev/null 2>&1; }

mk 1; mk 2; mk 3
start 1 a || { echo "node1 did not start"; exit 1; }
start 2 a || { echo "node2 did not start"; exit 1; }
sleep 6

# a genuine restart must still be admitted: same identity, same address
pkill -9 -f "[p]erfcached -f $D/n2.conf"; sleep 8
start 2 b || bad "a restarting node was not admitted"
sleep 5
alive 2 && ok "a genuine restart (same identity, same address) is admitted" \
	|| bad "a genuine restart was refused"

# A restart FASTER than its predecessor ages out must still be admitted,
# not shot for impersonating itself.  The master stays silent while the
# answer is ambiguous and the joiner retries.
pkill -9 -f "[p]erfcached -f $D/n2.conf"; sleep 1
start 2 c || bad "a fast restart never came up"
# Assert the EFFECT - it ends up a working member of a fleet that agrees -
# not a particular log line.  Killing the master and restarting it inside
# the detection window legitimately produces either path: it may join the
# survivor, or found its own and reconcile ("yielding mastership ...
# re-joining").  An earlier version of this grepped for "joined as node"
# and failed the second path, which is a correct outcome.
i=0
while [ $i -lt 45 ]; do
	alive 2 || break
	UP=$(./perfcli -h 127.0.50.2 -p 17962 -a $SEC -j '{"method":"stats"}' \
		2>/dev/null | python3 -c 'import json,sys
try:
    c = json.load(sys.stdin)["cluster"]
    print(1 if c["peers_up"] >= 1 and c["master"] else 0)
except Exception:
    print(0)' 2>/dev/null)
	[ "$UP" = 1 ] && break
	sleep 1; i=$((i+1))
done
alive 2 && [ "$UP" = 1 ] \
	&& ok "a restart faster than the purge window ends up a working member" \
	|| bad "a fast restart never rejoined the fleet (alive=$(alive 2 && echo y || echo n) up=${UP:-?})"

# now the clone: node3 gets node2's identity file, as a copied disk would
cp "$D/w2/node-identity" "$D/w3/node-identity"
start 3 a
# ambiguous for one grace window (3 x PEER_UP_MS), then condemned
sleep 30
if alive 3; then
	bad "the clone is still running - a duplicate identity was admitted"
else
	ok "the clone shut down"
fi
grep -q "already held by a live member" "$D/n3.a.log" 2>/dev/null \
	&& ok "it said why" || bad "no duplicate-identity message in its log"
grep -q "node-identity" "$D/n3.a.log" 2>/dev/null \
	&& ok "it named the fix" || bad "the message does not say how to fix it"
alive 1 && alive 2 && ok "the original and its peer are undisturbed" \
	|| bad "refusing the clone disturbed the running fleet"

echo "clonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
