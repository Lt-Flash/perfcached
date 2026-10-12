#!/bin/sh
# lanetest.sh - S36 step b: in discovery = unicast nothing larger than
# the path MTU goes as a datagram - it goes over the TCP frame lane.
#
# A cloud path MTU is ~1,460 bytes and some environments drop IP
# fragments; a 40 KB datagram is ~30 fragments, so one lost fragment
# (or a fragment filter) loses all of it.  The rig: three network
# namespaces on a bridge with MTU 1,400, and on every node's link a tc
# filter that drops the FIRST fragment of every fragmented packet (MF
# set) - no fragmented datagram can ever be reassembled:
#   1  lane on (the default, max_datagram = 1400): a fleet forms; a
#      40,000-byte value written on node A reads on B and C within a few
#      seconds (the eager push rode the lane); lane frames sent, none
#      dropped
#   2  the control, lane off (max_datagram = 0): the fleet cannot even
#      form - membership's own frames (~2.6 KB for three nodes) fragment
#      and are dropped - or, if it does, the 40 KB write does not
#      arrive; either way proof the rig drops fragments, so arm 1's pass
#      is the lane and not a lenient network
# Needs root, ip netns and tc; SKIPs loudly without them - a skip is not
# a pass.
# Usage: test/lanetest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
if [ "$(id -u)" != 0 ] || ! command -v ip >/dev/null 2>&1 ||
   ! command -v tc >/dev/null 2>&1; then
	echo "lanetest: SKIP - needs root, ip netns and tc - nothing was tested"
	exit 0
fi
R=pclr$$
D=$(mktemp -d /var/tmp/pclane.XXXXXX)
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
PORT=17940

# ---- the rig: a bridge, three nodes at MTU 1400, fragments dropped -----
ip netns add $R || { echo "lanetest: cannot make namespaces"; exit 1; }
ip -n $R link add br0 type bridge
ip -n $R link set br0 up
for n in 1 2 3; do
	ip netns add ${R}n$n
	ip link add v$n$$ type veth peer name b$n$$
	ip link set v$n$$ netns ${R}n$n
	ip link set b$n$$ netns $R
	ip -n $R link set b$n$$ master br0
	ip -n $R link set b$n$$ mtu 1400 up
	ip -n ${R}n$n link set lo up
	ip -n ${R}n$n link set v$n$$ mtu 1400 up
	ip -n ${R}n$n addr add 10.252.7.$n/24 dev v$n$$
	# drop the first fragment of every fragmented packet arriving here
	ip netns exec ${R}n$n tc qdisc add dev v$n$$ ingress
	ip netns exec ${R}n$n tc filter add dev v$n$$ parent ffff: protocol ip \
		u32 match u16 0x2000 0x2000 at 6 action drop
done

conf() { # conf <n> <max_datagram>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = ln-client-secret
cluster = ln-cluster-secret
[listen]
tcp = 127.0.0.1:17951
http = 127.0.0.1:17952
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 10.252.7.1
advertise = 10.252.7.$1
max_datagram = $2
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; rm -rf "$D/s$1"; mkdir -p "$D/s$1"
}
start() { ip netns exec ${R}n$1 "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
stopall() { for n in 1 2 3; do [ -f "$D/n$n.pid" ] && kill "$(cat "$D/n$n.pid")" 2>/dev/null; rm -f "$D/n$n.pid"; done; sleep 1; }
st() { # st <n> <expr over r>
	ip netns exec ${R}n$1 python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:17952/stats",timeout=5).read())
print(eval(sys.argv[1]))' "$2" 2>/dev/null || echo "?"
}
call() { # call <n> <json>
	ip netns exec ${R}n$1 python3 -c 'import json,socket,sys,pcnative
s=socket.create_connection(("127.0.0.1", 17951), 5); f=pcnative.wrap(s)
f.write(sys.argv[1].encode()+b"\n"); f.flush(); print(json.dumps(json.loads(f.readline()).get("result")))' "$2" 2>/dev/null
}
bigset() { # bigset <n> <key>: a 40,000-byte value
	ip netns exec ${R}n$1 python3 -c 'import json,socket,sys,pcnative
s=socket.create_connection(("127.0.0.1", 17951), 5); f=pcnative.wrap(s)
f.write(json.dumps({"method":"set","params":{"col":"0","key":sys.argv[1],"value":"L"*40000}}).encode()+b"\n"); f.flush()
print(json.loads(f.readline()).get("result"))' "$2" 2>/dev/null
}
biglen() { # biglen <n> <key>: the length read back, -1 = miss
	ip netns exec ${R}n$1 python3 -c 'import json,socket,sys,pcnative
s=socket.create_connection(("127.0.0.1", 17951), 5); f=pcnative.wrap(s)
f.write(json.dumps({"method":"get","params":{"col":"0","key":sys.argv[1]}}).encode()+b"\n"); f.flush()
r=json.loads(f.readline()).get("result") or {}
print(len(r.get("value","")) if r.get("found") else -1)' "$2" 2>/dev/null || echo -2
}
formed() { [ "$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')$(st 3 'r["cluster"]["peers_up"]')" = 222 ] &&
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ]; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

echo "--- 1: lane on - big frames by TCP, nothing fragments"
for n in 1 2 3; do conf $n 1400; start $n; sleep 0.5; done
wait_for 40 formed && ok "three nodes formed across the MTU-1400 bridge" \
	|| bad "no fleet (peers $(st 1 'r["cluster"]["peers_up"]')/$(st 2 'r["cluster"]["peers_up"]')/$(st 3 'r["cluster"]["peers_up"]'))"
bigset 1 big1 > /dev/null
wait_for 6 '[ "$(biglen 2 big1)" = 40000 ] && [ "$(biglen 3 big1)" = 40000 ]' \
	&& ok "a 40,000-byte value written on node 1 reads on nodes 2 and 3" \
	|| bad "the big value did not cross (node 2: $(biglen 2 big1), node 3: $(biglen 3 big1))"
LS=$(st 1 'r["cluster"]["lane"]["sent_frames"]'); LD=$(st 1 'r["cluster"]["lane"]["dropped_full"] + r["cluster"]["lane"]["dropped_stale"]')
[ "${LS:-0}" -gt 0 ] 2>/dev/null && [ "$LD" = 0 ] && ok "node 1 sent $LS frame(s) by the lane, none dropped" \
	|| bad "lane figures on node 1: sent $LS, dropped $LD"
stopall

echo "--- 2: the control - lane off, every frame a datagram"
for n in 1 2 3; do conf $n 0; start $n; sleep 0.5; done
if wait_for 40 formed; then
	bigset 1 big2 > /dev/null
	sleep 6
	B2=$(biglen 2 big2); B3=$(biglen 3 big2)
	[ "$B2" = -1 ] && [ "$B3" = -1 ] \
		&& ok "without the lane the 40,000-byte value did not cross - the rig drops fragments, so arm 1 was the lane" \
		|| bad "the big value crossed without the lane (node 2: $B2, node 3: $B3) - the rig does not drop fragments and proves nothing"
else
	# membership itself sends frames over the MTU (the join's answer
	# and the map are ~2.6 KB for three nodes): with them fragmented
	# and dropped, a fleet cannot form at all - the cloud failure the
	# lane exists for, and proof the rig drops what it should
	ok "without the lane the fleet cannot even form - its ~2.6 KB membership frames fragment and are dropped (arm 1 formed on the same rig)"
fi
stopall

grep -hE "ERROR|CRIT" "$D"/n*.log | head -5
echo "lanetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
