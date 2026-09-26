#!/bin/sh
# routedpairtest.sh - S211: the delete behind a routed jset takes the
# same road as the jset.
#
# A three-node eager fleet and the routing client (routedpairtest.c):
# with per-key routing on, a jset went to the key's owner over its own
# link while the jdel behind it went over the primary link, where the
# owner's push had not landed yet - the delete removed nothing and the
# document then arrived on every member.  The C program proves the pair
# on every member through per-node handles; this driver only brings the
# fleet up and counts.  Before the fix it fails the two routed legs and
# passes the routing-off pair and the positive control.
# Usage: test/routedpairtest.sh [./perfcached] [./routedpairtest]
set -u
BIN=${1:-./perfcached}
RT=${2:-./routedpairtest}
SEC=rp-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcrp.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill "$p"; done; rm -rf "$D"' EXIT INT TERM

mk() { # mk <node>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = rp-cluster-secret
[listen]
tcp = 127.0.53.$1:1806$1
[cluster]
multicast = 239.255.77.165:17265
advertise = 127.0.53.$1
[collection c]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.log"; return 1
}
peers_up() { printf '%s\n' '{"method":"stats"}' | timeout 30 ./perfcli -h 127.0.53.$1 -p 1806$1 -a $SEC -q 2>/dev/null | head -1 | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up", -1))
except Exception: print(-1)'; }
listed() { printf '%s\n' '{"method":"members"}' | timeout 30 ./perfcli -h 127.0.53.$1 -p 1806$1 -a $SEC -q 2>/dev/null | head -1 | python3 -c 'import json,sys
try: print(sum(1 for m in (json.load(sys.stdin).get("members") or []) if m.get("port")))
except Exception: print(-1)'; }

[ -x ./perfcli ] || { echo "routedpairtest: ./perfcli is missing (make perfcli) - the fleet cannot be read"; exit 1; }
for i in 1 2 3; do mk $i; done
for i in 1 2 3; do start $i || exit 1; done
i=0
while [ $i -lt 100 ]; do
	[ "$(peers_up 1)" = 2 ] && [ "$(peers_up 2)" = 2 ] && [ "$(peers_up 3)" = 2 ] && break
	sleep 0.2; i=$((i + 1))
done
[ "$(peers_up 1)" = 2 ] || { bad "the fleet did not form"; exit 1; }
# and every node must LIST all three with a client port: that is what
# the routing handle learns, and it arrives a beat after peers_up
i=0
while [ $i -lt 100 ]; do
	[ "$(listed 1)" = 3 ] && [ "$(listed 2)" = 3 ] && [ "$(listed 3)" = 3 ] && break
	sleep 0.2; i=$((i + 1))
done
[ "$(listed 1)" = 3 ] && ok "three eager members formed, each listing three with a client port" \
	|| { bad "the members list is short on a node ($(listed 1)/$(listed 2)/$(listed 3))"; exit 1; }

"$RT" 127.0.53.1 18061 "$SEC" > "$D/rt.out" 2>&1
RTRC=$?
sed -n 's/^ok: /  ok   /p;s/^FAIL: /  FAIL /p' "$D/rt.out"
N_OK=$(grep -c "^ok: " "$D/rt.out" 2>/dev/null); N_OK=${N_OK:-0}
N_BAD=$(grep -c "^FAIL: " "$D/rt.out" 2>/dev/null); N_BAD=${N_BAD:-0}
case "$N_OK" in ''|*[!0-9]*) N_OK=0;; esac
case "$N_BAD" in ''|*[!0-9]*) N_BAD=0;; esac
pass=$((pass + N_OK)); fail=$((fail + N_BAD))
# a client that died before its summary is a failure, not a clean count
grep -q "^routedpairtest: " "$D/rt.out" || { bad "the client did not finish (exit $RTRC): $(tail -2 "$D/rt.out" | tr '\n' ' ')"; }

echo "routedpairtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
