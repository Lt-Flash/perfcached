#!/bin/sh
# tombholetest.sh - 12ny AT-6 (AT-12 d): a lost delete datagram is a hole the
# member asks for, and nothing past it is forgotten until it is filled.
#
# A delete goes to each member as one unicast datagram.  One lost - a full
# socket, a dropped packet - was lost for good when the member never went
# silent: the catch-up replay is for a peer back from a SILENCE, and the
# repair sweep re-sends records, not deletes, so the member kept the key.
# With numbering, the member sees the hole: its mark stops below it (so no
# origin forgets past it) and it asks for [from, to] (M_GAP_REQ); the
# origin re-sends what it holds as numbered tombstones.
# Three eager nodes; node 3 runs under netcutshim, and for 400 ms it hears
# nothing from node 1 while node 1 deletes 20 keys a second:
#   1. the cut was delivered (the shim consumed node 1's datagrams)
#   2. node 3 asked for the hole and node 1 re-sent what it held
#   3. node 3 confirms every delete node 1 numbered - its mark passed the
#      hole - and node 1 forgot them all
#   4. node 3 holds none of the deleted keys
# FAIL-FIRST: before AT-6 the hole stays - node 3's mark stops below it,
# node 1 keeps every delete from the hole on, and node 3 keeps the keys
# whose deletes it lost.
# Usage: test/tombholetest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pcthole.XXXXXX)
PIDS="" DP=""
trap 'kill $DP 2>/dev/null; for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombholetest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19431 19432 19433; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombholetest: port $p already bound" >&2; exit 1; }
done
: > "$D/cut3"
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = th-client-secret
cluster = th-cluster-secret
[listen]
tcp = 127.0.149.$i:1943$i
plaintext = loopback
[cluster]
multicast = 239.255.77.167:17287
advertise = 127.0.149.$i
mode = eager
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
	if [ $i = 3 ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/cut3" PC_NETCUT_LOG="$D/cutlog3" \
			"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	else
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	fi
	PIDS="$PIDS $!"
done
for i in 1 2 3; do
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log" | tr '\n' ' ')"; exit 1; }
done
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.149.%d" % i, 19430 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
k=0; while [ $k -lt 75 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
python3 "$T/waitready.py" 127.0.149.1:19431 127.0.149.2:19432 127.0.149.3:19433 || bad "a node is not ready"
[ "$(st 1 'c["acks"]["fleet"]["confirming"]')" = True ] || { bad "the fleet does not confirm: $(st 1 'c.get("acks")')"; exit 1; }

python3 - <<'PY' > "$D/driver" 2>&1 &
import socket, time, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.149.1", 19431), 10))
t0 = time.time(); i = 0
while time.time() - t0 < 10:
    t = time.time()
    pcnative.call(f, "set", col="c", key="h:%d" % i, value="v")
    pcnative.call(f, "del", col="c", key="h:%d" % i)
    i += 1
    time.sleep(max(0, 1 / 20.0 - (time.time() - t)))
print(i)
PY
DP=$!
sleep 4
echo "cut 127.0.149.1" > "$D/cut3"
sleep 0.4
: > "$D/cut3"
wait $DP; DP=
N=$(tail -1 "$D/driver")

# 1
IN=$(sed -n 's/.*in \([0-9]*\).*/\1/p' "$D/cutlog3" 2>/dev/null)
[ "${IN:-0}" -gt 0 ] 2>/dev/null && ok "1. the cut was delivered: node 3's shim consumed $IN datagram(s) from node 1" ||
	bad "1. the cut never fired (shim log: $(cat "$D/cutlog3" 2>/dev/null)) - nothing below is a test of it"

# 2 and 3: the hole asked for and filled, the mark past it, the table empty
O1=$(st 1 "c['acks']['marks'][0]['origin']")
I1=$(st 1 "c['acks']['marks'][0]['incarn']")
k=0; while [ $k -lt 75 ]; do
	M1=$(st 1 "c['acks']['marks'][0]['confirmed']")
	M3=$(st 3 "next((m['confirmed'] for m in c['acks']['marks'] if m['origin'] == '$O1' and m['incarn'] == $I1), -1)")
	E1=$(st 1 'c["tombstones_retained"]["numbered"]')
	[ "$M3" = "$M1" ] && [ "$E1" = 0 ] && break
	sleep 0.2; k=$((k+1))
done
A3=$(st 3 'c["acks"]["gap_asked"]'); S1=$(st 1 'c["acks"]["gap_served"]'); R1=$(st 1 'c["acks"]["gap_resent"]')
[ "${A3:-0}" -ge 1 ] 2>/dev/null && [ "${S1:-0}" -ge 1 ] 2>/dev/null && [ "${R1:-0}" -ge 1 ] 2>/dev/null &&
	ok "2. node 3 asked for the hole ($A3 request(s)) and node 1 re-sent $R1 delete(s) in $S1 answer(s)" ||
	bad "2. gap requests: node 3 asked ${A3:-?}, node 1 answered ${S1:-?} and re-sent ${R1:-?}"
[ "$M3" = "$M1" ] && [ "$E1" = 0 ] &&
	ok "3. node 3 confirms all $M1 of node 1's numbered deletes - past the hole - and node 1 forgot them all" ||
	bad "3. node 3 confirms $M3 of node 1's $M1, node 1 still holds $E1 numbered: $(st 3 'c["acks"]')"

# 4
L=$(python3 - "$N" <<'PY' 2>/dev/null
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.149.3", 19433), 10))
n = 0
for i in range(int(sys.argv[1])):
    if (pcnative.call(f, "get", col="c", key="h:%d" % i).get("result") or {}).get("found"):
        n += 1
print(n)
PY
)
[ "$L" = 0 ] && ok "4. node 3 holds none of the $N keys deleted around the cut" ||
	bad "4. node 3 holds ${L:-?} of the $N deleted keys"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombholetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
