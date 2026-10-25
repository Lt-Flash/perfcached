#!/bin/sh
# tombmcasttest.sh - 12ny AT-9: on a multicast fleet a delete goes to the
# group ONCE; a unicast fleet, a long key and data_multicast = off keep one
# copy a peer.
#
# Before: every delete went out one sealed copy a peer, N-1 datagrams, also
# where the group reaches every member.  With the origin's numbering (AT-2)
# a copy the group loses is a hole the member asks for (AT-6), so while the
# fleet confirms (AT-10's gate) the group carries it.  Three eager nodes:
#   1. multicast: 100 deletes through node 1 cost 100 datagrams, all on the
#      group; node 1 drops its own copies the group loops back; nodes 2 and
#      3 hold none of the 100 and all of the 100 kept
#   2. a 2,000-byte key's delete (past one datagram) goes one copy a peer
#   3. node 1 restarted with data_multicast = off: 50 deletes, 2 datagrams
#      each, none on the group, and nodes 2 and 3 still hold none of them
#   4. a unicast fleet (discovery = unicast): 50 deletes, 2 datagrams each
# FAIL-FIRST: before AT-9 every delete cost 2 datagrams on three nodes, and
# cluster.tomb_group / tomb_dgrams do not exist.
# Usage: test/tombmcasttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctmcast.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19481 19482 19483 19484; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombmcasttest: port $p already bound" >&2; exit 1; }
done
# conf <i> <state suffix> <multicast|unicast> <auto|off>
conf() {
	mkdir -p "$D/w$1$2" "$D/s$1$2"
	if [ "$3" = multicast ]; then
		DISC="multicast = 239.255.77.170:17290"
	else
		DISC="discovery = unicast
port = 19484
seeds = 127.0.152.1"
	fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1$2
[memory]
arena_mb = 64
[secrets]
client = tm-client-secret
cluster = tm-cluster-secret
[listen]
tcp = 127.0.152.$1:1948$1
plaintext = loopback
[cluster]
$DISC
advertise = 127.0.152.$1
mode = eager
collections = c
data_multicast = $4
[wal]
dir = $D/w$1$2
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 & eval "P$1=$!"; PIDS="$PIDS $!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
stopn() { eval "p=\$P$1"; kill "$p" 2>/dev/null; k=0; while kill -0 "$p" 2>/dev/null && [ $k -lt 100 ]; do sleep 0.1; k=$((k+1)); done; }
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.152.%d" % i, 19480 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <set|del> <prefix> <from> <to>, through node 1
	python3 - "$@" <<'PY'
import socket, sys, pcnative
op, pre, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = pcnative.wrap(socket.create_connection(("127.0.152.1", 19481), 10))
for i in range(a, b):
    k = pre if pre.startswith("LONG") else "%s:%d" % (pre, i)
    if op == "set":
        pcnative.call(f, "set", col="c", key=k, value="v")
    else:
        pcnative.call(f, "del", col="c", key=k)
PY
}
held() { # held <node> <prefix> <from> <to>
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, pre, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = pcnative.wrap(socket.create_connection(("127.0.152.%d" % n, 19480 + n), 10))
def k(i): return pre if pre.startswith("LONG") else "%s:%d" % (pre, i)
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key=k(i)).get("result") or {}).get("found")))
PY
}
gate() { # the fleet confirms: 2 peers up and the acks gate open on node 1
	k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
	[ "$(st 1 'c["acks"]["fleet"]["confirming"]')" = True ] || bad "$1: the fleet never confirmed (peers $(st 1 'c.get("peers_up")'), waiting for $(st 1 'c["acks"]["fleet"]["waiting_for"]'))"
}
ctr() { st 1 "'%d %d %d %d' % (c['tomb_sent'], c['tomb_group'], c['tomb_dgrams'], c['tomb_own'])"; }
d4() { # d4 "<before>" "<after>": the four deltas
	set -- $1 $2; echo "$(($5 - $1)) $(($6 - $2)) $(($7 - $3)) $(($8 - $4))"; }
settle() { # settle <prefix> <from> <to>: until nodes 2 and 3 hold none
	k=0; while [ $k -lt 50 ] && [ "$(held 2 "$1" "$2" "$3") $(held 3 "$1" "$2" "$3")" != "0 0" ]; do sleep 0.2; k=$((k+1)); done
	echo "$(held 2 "$1" "$2" "$3") $(held 3 "$1" "$2" "$3")"; }

# ---- the multicast fleet
for i in 1 2 3; do conf $i a multicast auto; done
start 1; start 2; start 3
gate "1"

# 1. 100 deletes
ops set k 0 200
sleep 1
B=$(ctr); ops del k 0 100; H=$(settle k 0 100); A=$(ctr)
set -- $(d4 "$B" "$A")
[ "$1" = 100 ] && [ "$2" = 100 ] && [ "$3" = 100 ] && [ "$4" -ge 95 ] && [ "$H" = "0 0" ] &&
	[ "$(held 2 k 100 200) $(held 3 k 100 200)" = "100 100" ] &&
	ok "1. multicast: 100 deletes cost $3 datagrams, $2 on the group; $4 own copies came back and were dropped; nodes 2 and 3 hold none of them and all 100 kept" ||
	bad "1. multicast, 100 deletes: sent $1, on the group $2, datagrams $3 (want 100 100 100), own copies $4 (want 95 or more); nodes 2/3 hold $H of the deleted (want 0 0), $(held 2 k 100 200)/$(held 3 k 100 200) of the kept"

# 2. a long key: past one datagram, one copy a peer
LK="LONG$(python3 -c 'print("x" * 1996)')"
ops set "$LK" 0 1; sleep 1
B=$(ctr); ops del "$LK" 0 1; H=$(settle "$LK" 0 1); A=$(ctr)
set -- $(d4 "$B" "$A")
[ "$1" = 1 ] && [ "$2" = 0 ] && [ "$3" = 2 ] && [ "$H" = "0 0" ] &&
	ok "2. a 2,000-byte key's delete went one copy a peer ($3 datagrams, none on the group), and reached both" ||
	bad "2. the long key: sent $1, on the group $2 (want 0), datagrams $3 (want 2); nodes 2/3 hold $H (want 0 0)"

# 3. data_multicast = off on node 1
stopn 1; conf 1 a multicast off; start 1
gate "3"
sleep 1
B=$(ctr); ops del k 100 150; H=$(settle k 100 150); A=$(ctr)
set -- $(d4 "$B" "$A")
[ "$1" = 50 ] && [ "$2" = 0 ] && [ "$3" = 100 ] && [ "$H" = "0 0" ] &&
	ok "3. data_multicast = off: 50 deletes, $3 datagrams, none on the group; nodes 2 and 3 hold none" ||
	bad "3. data_multicast = off, 50 deletes: sent $1, on the group $2 (want 0), datagrams $3 (want 100); nodes 2/3 hold $H"

# 4. a unicast fleet
for i in 1 2 3; do stopn $i; done
for i in 1 2 3; do conf $i b unicast auto; done
start 1; start 2; start 3
gate "4"
ops set u 0 50; sleep 1
B=$(ctr); ops del u 0 50; H=$(settle u 0 50); A=$(ctr)
set -- $(d4 "$B" "$A")
[ "$1" = 50 ] && [ "$2" = 0 ] && [ "$3" = 100 ] && [ "$H" = "0 0" ] &&
	ok "4. a unicast fleet: 50 deletes, $3 datagrams (one copy a peer), none on the group; nodes 2 and 3 hold none" ||
	bad "4. unicast fleet, 50 deletes: sent $1, on the group $2 (want 0), datagrams $3 (want 100); nodes 2/3 hold $H"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombmcasttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
