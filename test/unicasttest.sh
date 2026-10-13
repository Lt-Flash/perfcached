#!/bin/sh
# unicasttest.sh - S36 step a: [cluster] discovery = unicast - a fleet
# with no multicast at all, found through seeds.
#
# Seven sends went to the multicast group (beats, joins, maps,
# goodbyes); in unicast mode they go to every seed and every known
# address.  A member that hears an address its master does not know
# tells it (M_PEER_HINT), and a master answers a hinted address at once,
# so a joiner that knows only one member still reaches the master.
#   1  three nodes, node 1 the only seed and started LAST: one cluster,
#      one id, every node sees two peers - and no node has a multicast
#      receive socket (the wildcard bind of the cluster port)
#   2  an eager write on node 3 reads on node 1: the data planes ride
#      the same unicast as before
#   3  kill -9 the master: the other two elect, same cluster id; the
#      killed node restarts and rejoins
#   4  a fourth node whose ONLY seed is a member that is not the master
#      joins: the member hints the master, the master answers
# Usage: test/unicasttest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcuni.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=17740
for p in $PORT 17741 17751 17752 17753 17754 17761 17762 17763 17764; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "unicasttest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <seeds>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = un-client-secret
cluster = un-cluster-secret
[listen]
tcp = 127.0.0.1:$((17750 + $1))
http = 127.0.0.1:$((17760 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = $2
advertise = 127.0.31.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { # start <n>
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
}
stop9() { kill -9 "$(cat "$D/n$1.pid")" 2>/dev/null; rm -f "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (17760 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
cid() { st "$1" 'r["cluster"]["cluster_id"]'; }
role() { st "$1" 'r["cluster"]["role"]'; }
call() { # call <n> <json>
	python3 -c 'import json,pcnative,socket,sys
s=socket.create_connection(("127.0.0.1", 17750 + int(sys.argv[1])), 5); f=pcnative.wrap(s)
f.write(sys.argv[2].encode()+b"\n"); f.flush(); print(json.dumps(json.loads(f.readline()).get("result")))' "$1" "$2" 2>/dev/null
}
wait_for() { # wait_for <secs> <shell condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

echo "--- 1: three nodes, the only seed started last"
conf 1 ""; conf 2 127.0.31.1; conf 3 127.0.31.1
start 3; sleep 3; start 2; sleep 3; start 1
ready() { st "$1" 'r["state"]'; }
wait_for 30 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(cid 1)" = "$(cid 2)" ] && [ "$(cid 2)" = "$(cid 3)" ] && [ "$(ready 1)$(ready 2)$(ready 3)" = readyreadyready ]' \
	&& ok "one cluster, one id ($(cid 1)), two peers on every node, all ready" \
	|| bad "no single fleet: peers $(up 1)/$(up 2)/$(up 3), ids $(cid 1) / $(cid 2) / $(cid 3)"
[ "$(st 1 'r["cluster"]["discovery"]["mode"]')" = unicast ] && ok "/stats says discovery unicast" \
	|| bad "discovery mode is $(st 1 'r["cluster"]["discovery"]["mode"]')"
MC=0 RUN=0
for n in 1 2 3; do
	pid=$(cat "$D/n$n.pid")
	kill -0 "$pid" 2>/dev/null && RUN=$((RUN + 1))
	# the multicast receive socket is the wildcard bind of the cluster port
	ss -ulnp 2>/dev/null | grep "pid=$pid," | grep -qE "(0\.0\.0\.0|\*):$PORT[[:space:]]" && MC=$((MC + 1))
done
[ $RUN = 3 ] && [ $MC = 0 ] && ok "no node opened a multicast receive socket (all three running)" \
	|| bad "$MC node(s) have a wildcard socket on the cluster port ($RUN of 3 running)"

echo "--- 2: eager replication over unicast"
getv() { call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"0\",\"key\":\"$2\"}}"; }
W=$(call 3 '{"method":"set","params":{"col":"0","key":"k1","value":"v1"}}')
wait_for 5 'getv 1 k1 | grep -q "\"v1\""' \
	&& ok "a write on node 3 reads on node 1" || bad "the write on node 3 ($W) did not reach node 1: $(getv 1 k1)"

echo "--- 3: the master dies; the rest elect; it comes back"
M=; for n in 1 2 3; do [ "$(role $n)" = master ] && M=$n; done
C0=$(cid 1)
if [ -z "$M" ]; then bad "no master found"; else
	stop9 $M
	O=; for n in 1 2 3; do [ $n != $M ] && O="$O $n"; done
	wait_for 30 'nm=0; for n in $O; do [ "$(role $n)" = master ] && nm=$((nm+1)); done; [ $nm = 1 ]' \
		&& ok "node $M killed: one of the other two is master" || bad "no new master after node $M died"
	ID_OK=1; for n in $O; do [ "$(cid $n)" = "$C0" ] || ID_OK=0; done
	[ $ID_OK = 1 ] && ok "and the cluster id is unchanged" || bad "the id changed after the failover"
	start $M
	wait_for 40 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(cid $M)" = "$C0" ]' \
		&& ok "node $M restarted and rejoined, same id" \
		|| bad "node $M did not rejoin (peers $(up 1)/$(up 2)/$(up 3), id $(cid $M))"
fi

echo "--- 4: a joiner whose only seed is a member, not the master"
M=; S=; for n in 1 2 3; do r=$(role $n); [ "$r" = master ] && M=$n; [ "$r" = member ] && [ -z "$S" ] && S=$n; done
H0=$(st "${M:-1}" 'r["cluster"]["discovery"]["hints_recv"]')
conf 4 127.0.31.$S
start 4
wait_for 30 '[ "$(up 4)" = 3 ] && [ "$(cid 4)" = "$C0" ] && [ "$(up ${M:-1})" = 3 ]' \
	&& ok "node 4, seeded with member node $S only, joined (peers 3, same id)" \
	|| bad "node 4 did not join through member node $S (peers $(up 4), id $(cid 4))"
H1=$(st "${M:-1}" 'r["cluster"]["discovery"]["hints_recv"]')
[ "${H1:-0}" -gt "${H0:-0}" ] 2>/dev/null && ok "the master was told of it ($H0 -> $H1 hints)" \
	|| bad "the master received no hint ($H0 -> $H1)"

grep -hE "ERROR|CRIT" "$D"/n*.log | head -5
for f in "$D"/*.pid; do kill "$(cat "$f")" 2>/dev/null; done
echo "unicasttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
