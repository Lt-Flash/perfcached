#!/bin/sh
# mcastttltest.sh - S291: [cluster] multicast_ttl lets a fleet span a
# multicast router.
#
# Two nodes on two segments, a Linux multicast router between them - three
# network namespaces, two veth pairs, smcrouted forwarding the cluster
# group statically both ways and the kernel forwarding unicast:
#
#   A (10.251.1.2) --- R (10.251.1.1 | 10.251.2.1) --- B (10.251.2.2)
#
#   1  multicast_ttl = 2: the router forwards the cluster plane, and the
#      two nodes form one fleet (each sees the other)
#   2  multicast_ttl = 1 (the default): nothing crosses the router, and
#      each stays alone - the documented one-segment behaviour
#   3  ttl 2, the router restarted (2 s down): both stay members, no
#      silent purge - beats survive a router restart inside the dead-node
#      margins (6 s)
#
# Needs root, ip netns and smcroute (smcrouted); SKIPs loudly without
# them - a skip is NOT a pass.
# Usage: test/mcastttltest.sh [./perfcached]
set -u
BIN=$(readlink -f "${1:-./perfcached}")
if [ "$(id -u)" != 0 ] || ! command -v ip >/dev/null 2>&1 ||
   ! command -v smcrouted >/dev/null 2>&1; then
	echo "mcastttltest: SKIP - needs root, ip netns and smcroute (smcrouted) - nothing was tested"
	exit 0
fi
R=pcmr$$ A=pcma$$ B=pcmb$$
D=$(mktemp -d /var/tmp/pcmt.XXXXXX)
PA= PB= PR=
cleanup() {
	[ -n "$PA" ] && kill -9 $PA 2>/dev/null
	[ -n "$PB" ] && kill -9 $PB 2>/dev/null
	[ -n "$PR" ] && kill -9 $PR 2>/dev/null
	ip netns del $A 2>/dev/null; ip netns del $B 2>/dev/null; ip netns del $R 2>/dev/null
	rm -rf "$D"
}
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
GROUP=239.255.77.91

# ---- the topology -----------------------------------------------------
ip netns add $R && ip netns add $A && ip netns add $B || { echo "mcastttltest: cannot make namespaces"; exit 1; }
ip link add va$$ type veth peer name ra$$
ip link add vb$$ type veth peer name rb$$
ip link set va$$ netns $A; ip link set ra$$ netns $R
ip link set vb$$ netns $B; ip link set rb$$ netns $R
ip -n $A addr add 10.251.1.2/24 dev va$$; ip -n $A link set va$$ up; ip -n $A link set lo up
ip -n $B addr add 10.251.2.2/24 dev vb$$; ip -n $B link set vb$$ up; ip -n $B link set lo up
ip -n $R addr add 10.251.1.1/24 dev ra$$; ip -n $R link set ra$$ up
ip -n $R addr add 10.251.2.1/24 dev rb$$; ip -n $R link set rb$$ up; ip -n $R link set lo up
ip -n $A route add default via 10.251.1.1
ip -n $B route add default via 10.251.2.1
ip netns exec $R sysctl -qw net.ipv4.ip_forward=1
ip netns exec $R sysctl -qw net.ipv4.conf.all.rp_filter=0
cat > "$D/smc.conf" <<EOF
phyint ra$$ enable
phyint rb$$ enable
mroute from ra$$ group $GROUP to rb$$
mroute from rb$$ group $GROUP to ra$$
EOF
router_up() {
	ip netns exec $R smcrouted -n -N -f "$D/smc.conf" -I pcmr$$ -P "$D/smc.pid" -l notice \
		> "$D/smc.log" 2>&1 &
	PR=$!
	sleep 1
}

conf() { # conf <name> <advertise> <ttl>
	cat > "$D/$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/state-$1
[memory]
arena_mb = 16
[secrets]
client = mt-client-secret
cluster = mt-cluster-secret
[listen]
tcp = 127.0.0.1:17910
http = 127.0.0.1:17912
plaintext = loopback
[cluster]
multicast = $GROUP:17911
advertise = $2
multicast_ttl = $3
[collection 0]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/$1.conf"
}
start() { # start <ttl>
	conf a 10.251.1.2 $1; conf b 10.251.2.2 $1
	rm -rf "$D/state-a" "$D/state-b"
	ip netns exec $A "$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 & PA=$!
	ip netns exec $B "$BIN" -f "$D/b.conf" > "$D/b.log" 2>&1 & PB=$!
}
stop() {
	kill $PA $PB 2>/dev/null; wait $PA $PB 2>/dev/null; PA= PB=
}
peers() { # peers <ns> -> peers_up as this node sees it
	ip netns exec $1 python3 -c 'import json,urllib.request
try:
    print(json.loads(urllib.request.urlopen("http://127.0.0.1:17912/stats",timeout=3).read())["cluster"].get("peers_up",-1))
except Exception:
    print(-1)'
}
both_see() { # both_see <secs> -> 0 when each node sees the other
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		[ "$(peers $A)" = 1 ] && [ "$(peers $B)" = 1 ] && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

router_up

echo "--- 1: multicast_ttl = 2 across the router"
start 2
if both_see 20; then
	ok "the two segments form one fleet (each node sees the other)"
else
	bad "no fleet across the router (A sees $(peers $A), B sees $(peers $B))"
	tail -3 "$D/a.log"
fi
grep -q "multicast TTL 2" "$D/a.log" && ok "the node says its TTL and what routed links need" \
	|| bad "no TTL NOTICE in the log"
[ "$(ip netns exec $A python3 -c 'import json,urllib.request; print(json.loads(urllib.request.urlopen("http://127.0.0.1:17912/stats",timeout=3).read())["cluster"]["multicast_ttl"])')" = 2 ] \
	&& ok "/stats reports multicast_ttl 2" || bad "/stats does not report multicast_ttl 2"

echo "--- 3: the router restarts (2 s down), ttl 2"
kill $PR; wait $PR 2>/dev/null; PR=
sleep 2
router_up
sleep 8
[ "$(peers $A)" = 1 ] && [ "$(peers $B)" = 1 ] && ok "both stay members across the restart" \
	|| bad "membership lost across a 2 s router restart (A $(peers $A), B $(peers $B))"
if grep -q "went silent" "$D/a.log" "$D/b.log"; then
	bad "a node was purged as silent during the restart"
else
	ok "no silent purge - the beats survived inside the dead-node margin"
fi
stop

echo "--- 2: multicast_ttl = 1, the default"
start 1
sleep 12
[ "$(peers $A)" = 0 ] && [ "$(peers $B)" = 0 ] && ok "nothing crosses the router: each node stays alone" \
	|| bad "a ttl-1 fleet formed across a router (A $(peers $A), B $(peers $B))"
stop

echo "mcastttltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
