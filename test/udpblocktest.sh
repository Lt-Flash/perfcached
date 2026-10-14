#!/bin/sh
# udpblocktest.sh - S36 step e2: UDP blocked on a path, TCP open, is NOT
# a dead node (memberlist's TCP fallback probe).
#
# A cloud security group, a Kubernetes NetworkPolicy or a stale UDP
# conntrack entry can drop a peer's UDP while its TCP gets through; to
# beats-on-UDP that looked exactly like a dead node.  Now, before a
# silent peer is purged (or a master declared dead) the node asks TCP:
# an answer means "UDP is blocked here" - an ERROR, every frame to it by
# the lane, and a request that it send everything back by TCP too.
# The rig: three namespaces on a bridge; once the fleet has formed, node
# 3's namespace drops every incoming UDP datagram:
#   1  lane on: node 3 logs the UDP block, purges neither peer, nobody
#      elects; the others are asked for TCP; after the switch the fleet
#      is three and eager writes cross both ways
#      S329: then the block is lifted, and within lane_retry_s (2 s here)
#      every forced lane goes back to UDP - 0.4.6 never released one
#   2  the control, lane off: node 3 loses its peers - proof the rig
#      blocks UDP, so arm 1's pass is the fallback
# Needs root, ip netns and iptables; SKIPs loudly without them.
# Usage: test/udpblocktest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
if [ "$(id -u)" != 0 ] || ! command -v ip >/dev/null 2>&1 ||
   ! command -v iptables >/dev/null 2>&1; then
	echo "udpblocktest: SKIP - needs root, ip netns and iptables - nothing was tested"
	exit 0
fi
R=pcub$$
D=$(mktemp -d /var/tmp/pcub.XXXXXX)
cleanup() {
	for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done
	for n in 1 2 3; do ip netns del ${R}n$n 2>/dev/null; done
	ip netns del $R 2>/dev/null
	rm -rf "$D"
}
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18340

ip netns add $R || { echo "udpblocktest: cannot make namespaces"; exit 1; }
ip -n $R link add br0 type bridge
ip -n $R link set br0 up
for n in 1 2 3; do
	ip netns add ${R}n$n
	ip link add v$n$$ type veth peer name b$n$$
	ip link set v$n$$ netns ${R}n$n
	ip link set b$n$$ netns $R
	ip -n $R link set b$n$$ master br0
	ip -n $R link set b$n$$ up
	ip -n ${R}n$n link set lo up
	ip -n ${R}n$n link set v$n$$ up
	ip -n ${R}n$n addr add 10.252.8.$n/24 dev v$n$$
done

conf() { # conf <n> <max_datagram>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = ub-client-secret
cluster = ub-cluster-secret
[listen]
tcp = 127.0.0.1:18351
http = 127.0.0.1:18352
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 10.252.8.1, 10.252.8.2, 10.252.8.3
advertise = 10.252.8.$1
max_datagram = $2
lane_retry_s = 2
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; rm -rf "$D/s$1"; mkdir -p "$D/s$1"
}
start() { ip netns exec ${R}n$1 "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
stopall() { for n in 1 2 3; do [ -f "$D/n$n.pid" ] && kill "$(cat "$D/n$n.pid")" 2>/dev/null; rm -f "$D/n$n.pid"; done; sleep 1; }
st() {
	ip netns exec ${R}n$1 python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:18352/stats",timeout=5).read())
print(eval(sys.argv[1]))' "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
call() {
	ip netns exec ${R}n$1 python3 -c 'import json,socket,sys,pcnative
s=socket.create_connection(("127.0.0.1", 18351), 5); f=pcnative.wrap(s)
f.write(sys.argv[1].encode()+b"\n"); f.flush(); print(json.dumps(json.loads(f.readline()).get("result")))' "$2" 2>/dev/null
}
getv() { call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"0\",\"key\":\"$2\"}}"; }
setv() { call "$1" "{\"method\":\"set\",\"params\":{\"col\":\"0\",\"key\":\"$2\",\"value\":\"$3\"}}" > /dev/null; }
formed() { [ "$(up 1)$(up 2)$(up 3)" = 222 ] &&
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ]; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
block3() { ip netns exec ${R}n3 iptables -A INPUT -p udp -j DROP; }
unblock3() { ip netns exec ${R}n3 iptables -F INPUT; }

echo "--- 1: lane on - UDP into node 3 blocked after formation"
for n in 1 2 3; do conf $n 1400; start $n; sleep 0.5; done
wait_for 40 formed || bad "the fleet did not form"
for n in 1 2 3; do eval "L$n=\$(wc -l < \"$D/n$n.log\")"; done
block3
sleep 20
for n in 1 2 3; do eval "tail -n +\$((L$n + 1)) \"$D/n$n.log\"" > "$D/n$n.after"; done
grep -q "answers on TCP" "$D/n3.after" && ok "node 3 says its peers answer on TCP - UDP blocked, not dead" \
	|| bad "node 3 did not notice the UDP block"
grep -q "went silent" "$D/n3.after" && bad "node 3 purged a peer: $(grep -m1 'went silent' "$D/n3.after" | cut -c1-110)" \
	|| ok "node 3 purged neither peer"
grep -qE "elected master|claimed mastership" "$D"/n*.after \
	&& bad "an election happened after the block" || ok "nobody elected a new master"
grep -q "asks for TCP" "$D/n1.after" "$D/n2.after" && ok "the others were asked for TCP" || bad "no 'asks for TCP' on nodes 1 or 2"
wait_for 20 '[ "$(up 1)$(up 2)$(up 3)" = 222 ]' && ok "the fleet is three with node 3's UDP blocked" \
	|| bad "peers $(up 1)/$(up 2)/$(up 3) with the block"
setv 1 a1 from1; setv 3 a3 from3
wait_for 5 'getv 3 a1 | grep -q from1 && getv 1 a3 | grep -q from3' \
	&& ok "eager writes cross both ways (node 1 -> 3 by TCP, 3 -> 1)" \
	|| bad "writes did not cross: node 3 sees $(getv 3 a1), node 1 sees $(getv 1 a3)"
# S329: the block lifted, the lanes forced for it must come back to UDP
fz() { st "$1" 'r["cluster"]["lane"]["forced"]'; }
F0="$(fz 1)$(fz 2)$(fz 3)"
unblock3
if [ "$F0" != 000 ] && wait_for 15 '[ "$(fz 1)$(fz 2)$(fz 3)" = 000 ]'; then
	ok "S329: the block lifted, every forced lane went back to UDP (forced $F0 -> 000): $(grep -h -m1 'works again' "$D"/n*.log | sed 's/.*NOTICE: //' | cut -c1-80)"
else
	bad "S329: forced lanes $(fz 1)/$(fz 2)/$(fz 3) after the block lifted (were $F0, want some, then 000)"
fi
setv 3 b3 after3
wait_for 5 'getv 1 b3 | grep -q after3' && ok "and writes still cross after the switch back" \
	|| bad "after the switch back node 1 sees $(getv 1 b3)"
stopall; unblock3

echo "--- 2: the control - lane off, the same block"
for n in 1 2 3; do conf $n 0; start $n; sleep 0.5; done
wait_for 40 formed || bad "the lane-off fleet did not form"
L3=$(wc -l < "$D/n3.log")
block3
sleep 15
tail -n +$((L3 + 1)) "$D/n3.log" | grep -q "went silent" \
	&& ok "without the lane node 3 purged its peers - the rig blocks UDP, so arm 1 was the fallback" \
	|| bad "node 3 kept its peers without the lane - the rig does not block UDP and proves nothing"
stopall; unblock3

echo "udpblocktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
