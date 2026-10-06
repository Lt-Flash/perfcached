#!/bin/sh
# maxmemberstest.sh - S299: a fleet at its member limit refuses a new
# member OUT LOUD, and the refused node stops instead of founding a
# second cluster.
#
# Before S299 a full peer table made the master log a warning and answer
# the joiner NOTHING; a joiner reads silence as "no master" and founds its
# own cluster.  Here the limit is reached with [cluster] max_members = 2,
# which takes the same refusal path as the build's 256-peer bound.
#
#   1  two nodes form a fleet
#   2  a third is REFUSED: the master says so, the joiner says why and
#      exits non-zero, and it never claims mastership
#   3  the fleet is still two, and still serving
# Usage: test/maxmemberstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcmm.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

node() { # node <n> <tcp> <http>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 1
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 16
[secrets]
client = mm-client-secret
cluster = mm-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = 239.255.77.93:17940
advertise = 127.0.3.$1
max_members = 2
[collection 0]
buckets_log2 = 10
mode = eager
CONF
	chmod 600 "$D/n$1.conf"
}
members() { # members <http> -> live members in this node's view
	python3 -c 'import json,sys,urllib.request
d=json.loads(urllib.request.urlopen("http://127.0.0.1:%s/members" % sys.argv[1],timeout=5).read())
print(sum(1 for m in d.get("members",[]) if m.get("gone_s",-1) < 0))' "$1" 2>/dev/null || echo -1
}
node 1 17941 17942; node 2 17943 17944; node 3 17945 17946

echo "--- 1: two nodes form a fleet"
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
i=0
while [ $i -lt 60 ]; do
	[ "$(members 17942)" = 2 ] && [ "$(members 17944)" = 2 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$(members 17942)" = 2 ] && ok "two members, as configured" || bad "the fleet did not form ($(members 17942))"

echo "--- 2: a third is refused"
"$BIN" -f "$D/n3.conf" > "$D/n3.log" 2>&1 & P3=$!
k=0
while [ $k -lt 200 ]; do
	kill -0 $P3 2>/dev/null || break
	sleep 0.1; k=$((k+1))
done
if kill -0 $P3 2>/dev/null; then
	bad "the refused node is still running after 20 s"
	kill -9 $P3; P3=
else
	# it stops through the ordinary shutdown path, as the duplicate-
	# identity refusal does, so it exits 0 - which is what keeps
	# systemd's Restart=on-failure from re-running it into the same
	# refusal every second
	wait $P3; rc=$?; P3=
	ok "the refused node stopped by itself (rc $rc)"
fi
grep -q "REFUSED this node - the fleet is at its member limit" "$D/n3.log" \
	&& ok "it says why - the member limit" || { bad "no member-limit message on the joiner"; tail -4 "$D/n3.log"; }
if grep -qE "elected master|founded the cluster|claimed mastership" "$D/n3.log"; then
	bad "the refused node claimed mastership - a second cluster"
else
	ok "it never claimed mastership - no second cluster"
fi
grep -q "REFUSING join from 127.0.3.3 - the fleet is at its member limit (2" "$D/n1.log" "$D/n2.log" \
	&& ok "the master says it refused, and why" || bad "no refusal line on the master"

echo "--- 3: the fleet is still two"
[ "$(members 17942)" = 2 ] && [ "$(members 17944)" = 2 ] && ok "both nodes still see a fleet of two" \
	|| bad "the fleet changed ($(members 17942) / $(members 17944))"

kill $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null; P1= P2=
echo "maxmemberstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
