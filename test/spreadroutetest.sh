#!/bin/sh
# spreadroutetest.sh — S127 Z3/Z6: libperfd routes a `spread` fleet.
#
# WHY THIS IS ITS OWN SUITE.  spreadtest.sh drives everything through
# line-oriented JSON over a `plaintext = loopback` listener, because its
# assertions read per-collection entry counts and that is the cheapest
# client that can do it.  libperfd speaks the ENCRYPTED native wire and
# does a Noise handshake, which that listener refuses - so the client
# half of Z3 could not live there and was asserted only at the contract
# level (routing.mode / routing.replicas published).  This closes it:
# same fleet shape, encrypted door, a real libperfd.
#
# WHAT IT CATCHES.  libperfd only routes for a mode it RECOGNISES.
# Before S127 it knew `shard` and `store`; a spread fleet therefore
# turned client routing OFF entirely and every read paid a pull.  That
# is CORRECT - the daemon forwards, there are no MOVED redirects - so
# nothing fails loudly, throughput just quietly halves.  A contract-level
# check cannot see it; only a real client can.
#
# Usage: test/spreadroutetest.sh [./perfcached] [./failovertest]
set -u
BIN=${1:-./perfcached}
FT=${2:-./failovertest}
D=$(mktemp -d /var/tmp/pcsr.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

SECRET=sr-client-secret
MC=239.255.77.46
MP=17146

[ -x "$FT" ] || { echo "spreadroutetest: $FT not built - SKIP (this suite"
	echo "  asserts nothing without it, and says so rather than passing)"; exit 0; }

node() { # node <n>
	mkdir -p "$D/state.$1"
	cat > "$D/node.$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/state.$1
[memory]
arena_mb = 64
[secrets]
client = $SECRET
cluster = sr-cluster-secret
[listen]
tcp = 127.0.0.1:$((17450 + $1))
http = 127.0.0.1:$((18450 + $1))
http_allow = 127.0.0.0/8
[cluster]
multicast = $MC:$MP
advertise = 127.0.9.$1
pull_timeout_ms = 300
mode = spread
replicas = 2
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() { # start <n> <var>
	"$BIN" -f "$D/node.$1.conf" > "$D/node.$1.log" 2>&1 &
	eval "P$2=\$!"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/node.$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start:"; tail -4 "$D/node.$1.log"; return 1
}
cfield() { # cfield <http-port> <cluster field>
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    print(""); sys.exit(0)
print(d.get("cluster", {}).get(sys.argv[2], ""))' "$1" "$2" 2>/dev/null
}

node 1; node 2; node 3
start 1 1 || exit 1
start 2 2 || exit 1
start 3 3 || exit 1

tfield() { # tfield <http-port> <top-level field>
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    print(""); sys.exit(0)
print(d.get(sys.argv[2], ""))' "$1" "$2" 2>/dev/null
}

# Wait for EVERY node to be ready, not just the one we poll.  A routed
# client sends to the key's HOLDER, so a peer still RECOVERING refuses
# the write with "node is not READY" - which reads like a routing
# failure and is not one.  Checking only node 1 is how this suite first
# reported "routed write 0 failed" against a healthy fleet.
i=0
while [ $i -lt 200 ]; do
	u=$(cfield 18451 peers_up); m=$(cfield 18451 master)
	s1=$(tfield 18451 state); s2=$(tfield 18452 state); s3=$(tfield 18453 state)
	[ "$u" = "2" ] && [ -n "$m" ] && [ "$m" != "0" ] \
		&& [ "$s1" = "ready" ] && [ "$s2" = "ready" ] && [ "$s3" = "ready" ] && break
	sleep 0.2; i=$((i+1))
done
echo "  fleet states: $s1/$s2/$s3"
[ "$u" = "2" ] && ok "three-node spread fleet formed (master $m)" \
	|| { bad "fleet never formed (peers_up=$u master=$m)"; echo "spreadroutetest: $pass passed, $fail failed"; exit 1; }

# ---- the client, over the ENCRYPTED wire -----------------------------
OUT=$(timeout 120 "$FT" 127.0.0.1 17451 17452 17453 "$SECRET" route 2>&1)
echo "$OUT" | sed 's/^/    | /'

echo "$OUT" | grep -q "client is routing by key" \
	&& ok "libperfd ROUTES on a spread fleet (it would not before S127)" \
	|| bad "libperfd is not routing - mode_ok does not accept spread"
echo "$OUT" | grep -q "200 routed writes" \
	&& ok "200 routed writes accepted" || bad "routed writes failed"
# the reads matter twice over: they prove routing works AND that exact-K
# retention did not lose anything - a non-holder drops its copy, so a
# read that lands on one must still resolve through a holder
echo "$OUT" | grep -q "200 routed reads all hit" \
	&& ok "200 routed reads all hit (exact-K retention lost nothing)" \
	|| bad "a routed read missed - retention or routing dropped a record"

# ROUTE-MISSED counts requests whose owner was known but not connected.
# With spares=-1 the client opens every member, so a spread fleet should
# find a holder it is connected to for essentially every key.
RM=$(echo "$OUT" | sed -n 's/^ROUTE-MISSED //p' | tail -1)
[ -n "$RM" ] && [ "$RM" -le 20 ] \
	&& ok "the client reached a holder directly for nearly every key (route_missed=$RM of 400)" \
	|| bad "route_missed=${RM:-?} of 400 - the client is not finding holders"

echo "spreadroutetest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
