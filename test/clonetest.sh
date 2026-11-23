#!/bin/sh
# clonetest.sh - a node whose identity file was copied with the machine: a genuine restart is still
# admitted, a clone (same identity, other address) is refused, says why and exits.
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcclone.XXXXXX)
SEC=cl-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# daemons die by the pids this script started, each checked against its command line so a
# reused pid never hits a stranger; the name-bound pattern stays as a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

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
# auto derives the identity from the platform, so a copied file is no clone; this
# suite tests the file alone, where a duplicate identity is still refused
identity_source = file
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
	: > "$D/n$1.$2.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	PIDS="$PIDS $!"
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
# node 2 a member before its restarts: a joiner is CONNECTING until its links answer
st() { ./perfcli -h 127.0.50.$1 -p 1796$1 -a $SEC -q -j '{"method":"stats"}' 2>/dev/null \
	| python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
i=0
while [ $i -lt 300 ] && { [ "$(st 1)" != "ready 1" ] || [ "$(st 2)" != "ready 1" ]; }; do
	sleep 0.1; i=$((i+1))
done

# a genuine restart must still be admitted: same identity, same address
pkill -9 -f "[p]erfcached -f $D/n2.conf"; sleep 8
start 2 b || bad "a restarting node was not admitted"
sleep 5
alive 2 && ok "a genuine restart (same identity, same address) is admitted" \
	|| bad "a genuine restart was refused"

# a restart FASTER than its predecessor ages out must still be admitted: the master stays silent
# while the answer is ambiguous and the joiner retries
pkill -9 -f "[p]erfcached -f $D/n2.conf"; sleep 1
start 2 c || bad "a fast restart never came up"
# assert the EFFECT, a working member of a fleet that agrees: a master restarted inside the
# detection window may join the survivor or found its own and reconcile - both are right
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
# ambiguous for one grace window (3 x PEER_UP_MS = 30 s), then condemned at the clone's next join
# retry: poll for the shutdown, up to 75 s (two windows and a retry)
i=0
while [ $i -lt 75 ]; do
	alive 3 || break
	sleep 1; i=$((i+1))
done
if alive 3; then
	bad "the clone is still running after ${i}s - a duplicate identity was admitted"
else
	ok "the clone shut down (after ${i}s)"
fi
grep -q "already held by a live member" "$D/n3.a.log" 2>/dev/null \
	&& ok "it said why" || bad "no duplicate-identity message in its log"
grep -q "node-identity" "$D/n3.a.log" 2>/dev/null \
	&& ok "it named the fix" || bad "the message does not say how to fix it"
# while the master holds the join it must SAY so, and the held joiner must not found a cluster of
# its own (a silent hold made the clone a second master)
grep -q "holding it to see" "$D/n1.a.log" "$D/n2.c.log" 2>/dev/null \
	&& ok "the master held the join out loud" || bad "no 'holding' line on the master"
grep -q "is holding this join" "$D/n3.a.log" 2>/dev/null \
	&& ok "the clone knew it was held" || bad "the clone never heard the hold"
grep -q "founded the cluster" "$D/n3.a.log" 2>/dev/null \
	&& bad "the clone FOUNDED its own cluster while held" || ok "the clone never founded"
alive 1 && alive 2 && ok "the original and its peer are undisturbed" \
	|| bad "refusing the clone disturbed the running fleet"

echo "clonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
