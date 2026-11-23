#!/bin/sh
# tombsupertest.sh - a hole over deletes the origin has since superseded (newer deletes of the same
# keys, which it holds) is floored with no rebuild, even when the origin cannot rule out a loss in that
# space by its loss table (node 1's starts full, as past SPL_MAX spaces): it remembers what it superseded.
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctsuper.XXXXXX)
PIDS="" DP=""
trap 'kill $DP 2>/dev/null; for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombsupertest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19811 19812 19813; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombsupertest: port $p already bound" >&2; exit 1; }
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
client = ts-client-secret
cluster = ts-cluster-secret
[listen]
tcp = 127.0.181.$i:1981$i
plaintext = loopback
[cluster]
multicast = 239.255.77.213:17333
advertise = 127.0.181.$i
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
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut3" PC_NETCUT_LOG="$D/cutlog3" \
			"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	elif [ $i = 1 ]; then
		PERFCACHED_TEST_FG_FULL=1 "$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
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
f = pcnative.wrap(socket.create_connection(("127.0.181.%d" % i, 19810 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
k=0; while [ $k -lt 75 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
python3 "$T/waitready.py" 127.0.181.1:19811 127.0.181.2:19812 127.0.181.3:19813 || bad "a node is not ready"
[ "$(st 1 'c["acks"]["fleet"]["confirming"]')" = True ] || { bad "the fleet does not confirm: $(st 1 'c.get("acks")')"; exit 1; }

# twenty keys deleted again and again through node 1, each every 0.4 s: whatever node 3 misses in a
# 0.4 s cut has been superseded by the time it asks (GAP_WAIT_MS), and node 1 still holds the newer ones
python3 - <<'PY' > "$D/driver" 2>&1 &
import socket, time, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.181.1", 19811), 10))
t0 = time.time(); i = 0
while time.time() - t0 < 12:
    t = time.time()
    pcnative.call(f, "set", col="c", key="r:%d" % (i % 20), value="v%d" % i)
    pcnative.call(f, "del", col="c", key="r:%d" % (i % 20))
    i += 1
    time.sleep(max(0, 1 / 50.0 - (time.time() - t)))
print(i)
PY
DP=$!
sleep 4
F0=$(st 1 'c["acks"]["gap_floored"]'); B0=$(st 1 'c["acks"]["rebuild_sent"]'); Q0=$(st 3 'c["acks"]["rebuilds"]')
echo "cut 127.0.181.1 rst" > "$D/cut3"
sleep 0.4
: > "$D/cut3"
wait $DP; DP=
sleep 3
# 1
IN=$(sed -n 's/.*in \([0-9]*\).*/\1/p' "$D/cutlog3" 2>/dev/null)
[ "${IN:-0}" -gt 0 ] 2>/dev/null && ok "1. the cut was delivered: node 3's shim consumed $IN datagram(s) from node 1" ||
	bad "1. the cut never fired (shim log: $(cat "$D/cutlog3" 2>/dev/null)) - nothing below is a test of it"
# 2: the hole was asked for and floored - the superseded path, not a re-send of held deletes alone
F1=$(st 1 'c["acks"]["gap_floored"]'); B1=$(st 1 'c["acks"]["rebuild_sent"]'); Q1=$(st 3 'c["acks"]["rebuilds"]')
A3=$(st 3 'c["acks"]["gap_asked"]')
[ "${F1:-0}" -gt "${F0:-0}" ] 2>/dev/null && ok "2. node 3 asked for the hole ($A3 request(s)) and node 1 floored it ($F0 -> $F1)" ||
	bad "2. node 1 floored nothing ($F0 -> $F1, node 3 asked ${A3:-?}) - the case below was not reached"
# 3: superseded is not lost - no rebuild asked, none run
[ "$B1" = "$B0" ] && [ "$Q1" = "$Q0" ] &&
	ok "3. no rebuild: node 1 asked for none ($B0 -> $B1), node 3 ran none ($Q0 -> $Q1)" ||
	bad "3. node 1 asked node 3 to rebuild for deletes it had superseded and holds ($B0 -> $B1 asked, $Q0 -> $Q1 run on node 3): $(grep -m2 'asked to rebuild\|deletes in question were lost' "$D/n1.log" "$D/n3.log" | cut -c1-200 | tr '\n' ' ')"
# 4: and the floor lost nothing - node 3 confirms node 1's space and holds none of the keys
O1=$(st 1 "c['acks']['marks'][0]['origin']")
I1=$(st 1 "c['acks']['marks'][0]['incarn']")
k=0; while [ $k -lt 75 ]; do
	M1=$(st 1 "c['acks']['marks'][0]['confirmed']")
	M3=$(st 3 "next((m['confirmed'] for m in c['acks']['marks'] if m['origin'] == '$O1' and m['incarn'] == $I1), -1)")
	[ "$M3" = "$M1" ] && break
	sleep 0.2; k=$((k+1))
done
L=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.181.3", 19813), 10))
print(sum(1 for i in range(20) if (pcnative.call(f, "get", col="c", key="r:%d" % i).get("result") or {}).get("found")))
PY
)
[ "$M3" = "$M1" ] && [ "$L" = 0 ] &&
	ok "4. node 3 confirms all $M1 of node 1's numbered deletes and holds none of the 20 deleted keys" ||
	bad "4. node 3 confirms ${M3:-?} of node 1's ${M1:-?} and holds ${L:-?} of the 20 deleted keys"
for i in 1 2 3; do
	grep -h 'an fsync has been stuck' "$D/n$i.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	grep "CRIT" "$D/n$i.log" | grep -qv 'an fsync has been stuck' &&
		bad "node $i logged CRIT: $(grep 'CRIT' "$D/n$i.log" | grep -v 'an fsync has been stuck' | head -n 2 | tr '\n' ' ' | cut -c1-220)"
done
echo "tombsupertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
