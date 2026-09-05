#!/bin/sh
# gossiptest.sh - S78: a member record carries the peer's HTTP port and
# uptime, gossiped in the heartbeat, so the fleet grid links to each
# node's OWN door instead of guessing from location.port, and can show
# per-node uptime.  Two nodes; only node 1 opens an HTTP door.
# usage: test/gossiptest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcgos.XXXXXX)
SEC=gs-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
for pf in 17981 17982 19981; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "gossiptest: port $pf already bound" >&2; exit 1; }
done
mk() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = gs-cluster-secret
[listen]
tcp = 127.0.57.$1:1798$1
$2
[cluster]
multicast = 239.255.77.171:17271
advertise = 127.0.57.$1
mode = store
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
members() { "$CLI" -h 127.0.57.$1 -p 1798$1 -a $SEC -q -j '{"method":"members"}'; }
field() { # field <json> <peer-node-addr> <field>
	echo "$1" | python3 -c '
import json,sys
d=json.load(sys.stdin); addr=sys.argv[1]; f=sys.argv[2]
for m in d["members"]:
    if m["addr"]==addr: print(m.get(f, "absent")); break
else: print("nomember")' "$2" "$3"
}
mk 1 "http = 127.0.57.1:19981"
mk 2 ""
start 1 || { echo "node 1 did not start"; exit 1; }
start 2 || { echo "node 2 did not start"; exit 1; }
sleep 3.5
M2=$(members 2)
H=$(field "$M2" 127.0.57.1 http)
[ "$H" = 19981 ] && ok "node 2 learned node 1's HTTP door from the heartbeat (http=$H)" \
	|| bad "node 2 sees node 1's http as '$H' (want 19981): $(echo "$M2" | cut -c1-200)"
U1=$(field "$M2" 127.0.57.1 uptime_s)
case "$U1" in ''|absent|nomember|-*) bad "node 2 sees node 1's uptime as '$U1'";;
	*) ok "node 2 sees node 1's uptime (${U1}s)";; esac
M1=$(members 1)
H2=$(field "$M1" 127.0.57.2 http)
[ "$H2" = 0 ] && ok "a node with no HTTP door gossips 0, not a guess" \
	|| bad "node 1 sees node 2's http as '$H2' (want 0)"
S1=$(field "$M1" 127.0.57.1 http)
[ "$S1" = 19981 ] && ok "the self record carries its own door" || bad "self http='$S1'"
sleep 2.5
U1b=$(field "$(members 2)" 127.0.57.1 uptime_s)
[ "$U1b" -gt "$U1" ] 2>/dev/null && ok "a peer's uptime advances between reads ($U1 -> $U1b)" \
	|| bad "peer uptime did not advance ($U1 -> $U1b)"
# the HTTP door serves the same record, and the grid reads it from there
W=$(curl -s --max-time 3 http://127.0.57.1:19981/members 2>/dev/null || python3 -c '
import urllib.request; print(urllib.request.urlopen("http://127.0.57.1:19981/members", timeout=3).read().decode())')
[ "$(field "$W" 127.0.57.2 http)" = 0 ] && [ "$(field "$W" 127.0.57.1 http)" = 19981 ] \
	&& ok "/members on the HTTP door carries http and uptime_s" \
	|| bad "/members over HTTP: $(echo "$W" | cut -c1-160)"
echo "gossiptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
