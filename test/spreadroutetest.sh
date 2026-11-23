#!/bin/sh
# spreadroutetest.sh - libperfd routes a `spread` fleet over the encrypted native wire: a client
# that did not recognise the mode would pay a pull on every read and fail nothing.
set -u
BIN=${1:-./perfcached}
FT=${2:-./failovertest}
D=$(mktemp -d /var/tmp/pcsr.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
	: > "$D/node.$1.log"
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

# wait for EVERY node to be ready: a routed client sends to the key's holder, and a peer still
# RECOVERING refuses with "node is not READY", which reads like a routing failure
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
# the reads prove routing works AND that exact-K retention lost nothing: a non-holder drops
# its copy, so a read landing on one must still resolve through a holder
echo "$OUT" | grep -q "200 routed reads all hit" \
	&& ok "200 routed reads all hit (exact-K retention lost nothing)" \
	|| bad "a routed read missed - retention or routing dropped a record"

# ROUTE-MISSED counts requests whose owner was known but not connected: with spares=-1 the
# client opens every member, so a spread fleet finds a connected holder for nearly every key
RM=$(echo "$OUT" | sed -n 's/^ROUTE-MISSED //p' | tail -1)
[ -n "$RM" ] && [ "$RM" -le 20 ] \
	&& ok "the client reached a holder directly for nearly every key (route_missed=$RM of 400)" \
	|| bad "route_missed=${RM:-?} of 400 - the client is not finding holders"

echo "spreadroutetest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
