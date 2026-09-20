#!/bin/sh
# spreadcoldtest.sh - S156: a spread node that comes back EMPTY must end
# up holding its share of the keyspace, not all of it.
#
# Measured before this existed (2026-09-15, three nodes, K=2, 3,000
# keys): the returning node was streamed the boot sender's WHOLE store
# and then the other holder's set-repair added the rest - 3,000 of 3,000
# within 18 s, fleet 7,006 where K x N is 6,000 - and the reclaim pass
# trimmed the surplus at 64 keys a sweep.  The boot pull (bulk_serve_boot
# / boot_cb) streamed every record of every eager collection with no
# regard for who holds what under spread.
#
# THE CLAIM: after a cold restart and settle, the returning node holds
# about its share (2/3 of N at K=2, P=3), the fleet holds about K x N,
# and every key is still readable through the returning node - the fix
# must not buy the bound with data.  Eager is the control: the same
# restart under eager must still hand the node EVERYTHING, or the scope
# leaked into the mode that has no shares.
#
# Usage: test/spreadcoldtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
N=${N:-3000}
D=$(mktemp -d /var/tmp/pcspreadcold.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

node() { # node <n> <cluster-block>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = info
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = spreadcold-client-secret
cluster = spreadcold-cluster-secret
[listen]
tcp = 127.0.0.1:1743$1
http = 127.0.0.1:1843$1
plaintext = loopback
[cluster]
multicast = 239.255.77.47:17147
advertise = 127.0.10.$1
pull_timeout_ms = 200
$2
collections = b
[collection b]
buckets_log2 = 12
pull = 1
CONF
}
start() { # start <n>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 120 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 never became ready"; tail -3 "$D/n$1.log"; return 1
}
stat() { # stat <n> <python expression over d>
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1843%s/stats" % sys.argv[1], timeout=4))
except Exception:
    print("?"); sys.exit(0)
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null
}
ent() { stat "$1" '[c.get("entries",0) for c in d.get("collections",[]) if c.get("name")=="b"][0]'; }
ready3() {
	i=0
	while [ $i -lt 150 ]; do
		a=$(stat 1 'str(d["cluster"]["peers_up"])+" "+d["state"]')
		b=$(stat 2 'str(d["cluster"]["peers_up"])+" "+d["state"]')
		c=$(stat 3 'str(d["cluster"]["peers_up"])+" "+d["state"]')
		[ "$a" = "2 ready" ] && [ "$b" = "2 ready" ] && [ "$c" = "2 ready" ] && return 0
		sleep 0.2; i=$((i+1))
	done
	echo "  fleet never formed: [$a] [$b] [$c]"; return 1
}
rpc() { # rpc <port>: JSON requests on stdin; prints "requests N hits H"
	timeout 120 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
f = s.makefile("rwb"); n = 0; hit = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    a = json.loads(f.readline()); n += 1
    if r["method"] == "get" and a.get("result", {}).get("found"): hit += 1
print("requests", n, "hits", hit)' "$1"
}
fill() { i=0; while [ $i -lt $N ]; do printf '{"method":"set","params":{"col":"b","key":"sk%05d","value":"v%05d","ttl":900}}\n' $i $i; i=$((i+1)); done | rpc "$1" >/dev/null; }
hits_via() { i=0; while [ $i -lt $N ]; do printf '{"method":"get","params":{"col":"b","key":"sk%05d"}}\n' $i; i=$((i+1)); done | rpc "$1" | awk '{print $4}'; }
settle() { # settle: wait until the fleet's entry counts hold still (or 90 s)
	prev=-1; st=0; w=0
	while [ $w -lt 45 ]; do
		E1=$(ent 1); E2=$(ent 2); E3=$(ent 3)
		SUM=$(( ${E1:-0} + ${E2:-0} + ${E3:-0} ))
		if [ "$SUM" = "$prev" ]; then st=$((st+1)); [ $st -ge 5 ] && return 0; else st=0; fi
		prev=$SUM; sleep 2; w=$((w+1))
	done
	return 0
}
fleet() { # fleet <label> <cluster-block>
	for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done
	P1= P2= P3=; rm -rf "$D"/s? "$D"/n?.log
	node 1 "$2"; node 2 "$2"; node 3 "$2"
	start 1 && start 2 && start 3 && ready3 || { echo "$1: fleet failed"; exit 1; }
}
restart2_cold() { # kill node 2 and bring it back with nothing
	kill -9 "$P2"; wait "$P2" 2>/dev/null; P2=
	rm -rf "$D/s2"; mkdir -p "$D/s2"
	start 2 || exit 1
	sleep 3
}
HI=$(( N * 80 / 100 )); LO=$(( N * 55 / 100 )); TWO=$(( N * 2 )); TWOMAX=$(( TWO * 105 / 100 ))

echo "== spread K=2, P=3, $N keys through node 1"
fleet spread "mode = spread
replicas = 2"
fill 17431; settle
echo "  after the fill: $E1/$E2/$E3 sum=$SUM (K x N = $TWO)"
[ "$SUM" -ge $(( TWO * 95 / 100 )) ] && [ "$SUM" -le $TWOMAX ] \
	&& ok "the fill placed K copies of every key (sum $SUM ~ $TWO)" \
	|| bad "the fill placed $SUM copies where K x N is $TWO - the fleet is not the rig this test assumes"
restart2_cold; settle
echo "  after node 2 came back empty and the fleet settled: $E1/$E2/$E3 sum=$SUM"
[ "${E2:-0}" -ge $LO ] && [ "${E2:-0}" -le $HI ] \
	&& ok "the returning node holds its share ($E2 of $N, want $LO..$HI)" \
	|| bad "the returning node holds $E2 of $N - its share is $LO..$HI; it was handed the keyspace, not its part of it"
[ "$SUM" -le $TWOMAX ] \
	&& ok "the fleet holds K copies, not more ($SUM <= $TWOMAX)" \
	|| bad "the fleet holds $SUM where K x N is $TWO - surplus copies after a cold start"
H=$(hits_via 17432)
[ "${H:-0}" -eq $N ] && ok "every key still readable through the returning node ($H of $N)" \
	|| bad "${H:-?} of $N keys readable through the returning node - the bound was bought with data"

echo "== eager control: the same restart must hand the node everything"
fleet eager "mode = eager"
fill 17431; settle
restart2_cold; settle
echo "  after node 2 came back empty under eager: $E1/$E2/$E3"
[ "${E2:-0}" -ge $(( N * 97 / 100 )) ] && ok "eager: the returning node holds everything ($E2 of $N)" \
	|| bad "eager: the returning node holds $E2 of $N - the spread scope leaked into eager"

echo "spreadcoldtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
