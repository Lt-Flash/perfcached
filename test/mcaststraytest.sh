#!/bin/sh
# mcaststraytest.sh - the multicast socket takes the group's datagrams only.
# Every node binds it to ANY:port beside every other daemon on the host
# (REUSEADDR), so a unicast datagram for a local address nothing is bound to
# - a node just killed, its restart not up yet - was delivered to one of
# them and handled as its own: failovertest's bounce saw a node serve its
# own forward that way, storing a key it does not own and acking it.  Now
# the datagram's destination (IP_PKTINFO) decides: not the group, dropped,
# counted in mcast_stray and said once a minute per sender.
# A two-node multicast fleet; an ALIVE sealed with the fleet's secret, sent
# by clinject from 127.0.146.9 to 127.0.146.9 - an address no node is bound
# to, so it reaches a node's multicast socket only.
#   1  a node counts it as a stray, and says so
#   2  no node takes the sender as a peer - before, the ALIVE made it one
# The group's own traffic is the control: the fleet forms and stays whole.
# Usage: test/mcaststraytest.sh [./perfcached] [./clinject]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcmstray.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=23500
SECRET=ms-cluster-secret
for p in $PORT 23551 23552 23561 23562 23599; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "mcaststraytest: port $p busy" >&2; exit 1; }
done
[ -x "$INJ" ] || { echo "mcaststraytest: $INJ not built" >&2; exit 1; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = ms-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:$((23550 + $1))
http = 127.0.0.1:$((23560 + $1))
plaintext = loopback
[cluster]
multicast = 239.255.146.1:$PORT
advertise = 127.0.146.$1
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (23560 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
wait_for() { # wait_for <secs> <shell condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
stray() { st "$1" 'r["cluster"]["mcast_stray"]'; }
# both nodes' strays, "?" while either is unreadable (a build without the
# counter reads "?", and dash dies on "?" in an arithmetic expansion)
strays() {
	a=$(stray 1); b=$(stray 2)
	case "$a$b" in ''|*[!0-9]*) echo "?";; *) echo $((a + b));; esac
}

conf 1; conf 2
start 1
wait_for 30 '[ "$(st 1 "r[\"cluster\"][\"role\"]")" = master ]' || bad "node 1 did not found the fleet"
start 2
wait_for 30 '[ "$(up 1)$(up 2)" = 11 ]' || bad "the fleet did not form: peers $(up 1)/$(up 2)"
CID=$(st 1 'r["cluster"]["cluster_id"]')
S0=$(strays)

echo "--- an ALIVE for 127.0.146.9, where no node is bound"
O=$("$INJ" -s "$SECRET" -c "$CID" -f 127.0.146.9:23599 -t 127.0.146.9:$PORT alive 88 2>&1)
echo "$O" | grep -q '^sent 1' || bad "the ALIVE was not sent: $O"
wait_for 5 'n=$(strays); [ "$n" != "?" ] && [ "$S0" != "?" ] && [ "$n" -gt "$S0" ]' \
	&& ok "1. counted as a stray on the multicast socket (mcast_stray $S0 -> $(strays) over both nodes)" \
	|| bad "1. mcast_stray $(stray 1)/$(stray 2), was $S0 in all ($O)"
grep -q "dropped a datagram from 127.0.146.9 for 127.0.146.9 on the multicast socket" "$D/n1.log" "$D/n2.log" \
	&& ok "1. and said who sent it and for whom" \
	|| bad "1. no stray line: $(grep -h 'multicast socket' "$D/n1.log" "$D/n2.log" | tail -1 | cut -c1-160)"

echo "--- 2: the sender is nobody's peer"
sleep 2
[ "$(up 1)$(up 2)" = 11 ] \
	&& ok "2. both nodes still count one peer each - the stray ALIVE made nobody a member" \
	|| bad "2. peers $(up 1)/$(up 2) after the stray ALIVE"
! grep -h "127.0.146.9" "$D/n1.log" "$D/n2.log" | grep -qv "dropped a datagram" \
	&& ok "2. no line names 127.0.146.9 but the drop" \
	|| bad "2. lines naming 127.0.146.9: $(grep -h '127.0.146.9' "$D/n1.log" "$D/n2.log" | grep -v 'dropped a datagram' | head -2 | cut -c1-160 | tr '\n' '|')"

echo "mcaststraytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
