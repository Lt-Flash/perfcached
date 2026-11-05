#!/bin/sh
# tombrebuildtest.sh - 12ny AT-8 (AT-12 g): a member away longer than the
# fleet keeps deletes for it is rebuilt, not waited for - and keeps what it
# wrote while it was away.
#
# The deletes a member missed are kept for it while it is counted and up to
# tombstone_retain_s; past that nobody holds them, and a replay or a gap
# request has nothing to give back.  A restart was already told (S392's
# fast path refuses and it walks).  A member back WITHOUT a restart - a
# partition healed - took the replay of what was left and kept the keys
# whose deletes were gone, and pushed them back to the fleet.  Now the
# nodes that no longer hold what it missed say so (M_REBUILD) and it
# reconciles every record written before its absence; its writes since are
# not in question.  Three eager nodes, tombstone_retain_s 20; node 3 under
# netcutshim is cut from both peers, multicast too, for 35 s:
#   1. during the cut node 1 deletes 200 of 300 keys, and node 3 takes 50
#      writes of its own (as far as it accepts them alone)
#   2. healed: node 1 or 2 asked node 3 to rebuild, node 3 took it, and
#      the asker saw it taken (one pass, or two when the askers' views of
#      when the absence began fall in different seconds)
#   3. node 3 holds none of the 200 deleted keys and all 100 that live
#   4. node 3 still holds every write it accepted during the cut - stored
#      after the absence began, so not in question
#   5. nodes 1 and 2 hold none of the 200 either: nothing node 3 kept came
#      back to them on the heal
# FAIL-FIRST: before AT-8 nobody asks - node 3 keeps the deleted keys whose
# deletes the time rule dropped while it was away.
# Usage: test/tombrebuildtest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctreb.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombrebuildtest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19471 19472 19473; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombrebuildtest: port $p already bound" >&2; exit 1; }
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
client = tr-client-secret
cluster = tr-cluster-secret
[listen]
tcp = 127.0.151.$i:1947$i
plaintext = loopback
[cluster]
multicast = 239.255.77.169:17289
advertise = 127.0.151.$i
mode = eager
collections = c
tombstone_retain_s = 20
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
T0=$(date +%s)
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.151.%d" % i, 19470 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <node> <set|del> <prefix> <from> <to>: prints how many succeeded
	python3 - "$@" <<'PY'
import socket, sys, pcnative
n, op, pre, a, b = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
f = pcnative.wrap(socket.create_connection(("127.0.151.%d" % n, 19470 + n), 10))
ok = 0
for i in range(a, b):
    if op == "set":
        r = pcnative.call(f, "set", col="c", key="%s:%d" % (pre, i), value="v")
    else:
        r = pcnative.call(f, "del", col="c", key="%s:%d" % (pre, i))
    ok += "error" not in r
print(ok)
PY
}
held() { # held <node> <prefix> <from> <to>: how many it holds
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, pre, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = pcnative.wrap(socket.create_connection(("127.0.151.%d" % n, 19470 + n), 10))
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key="%s:%d" % (pre, i)).get("result") or {}).get("found")))
PY
}
k=0; while [ $k -lt 75 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
# a write node 1 takes before any is counted on: v0.7.3-rc4 (GitHub core2)
# found node 3 holding none of the 300 below, the helper's count thrown
# away - and tombjointest's node 1 had taken none of its writes, the fleet
# formed by every figure it read.  The first refusal is said either way
probe() { # a write node 1 takes: "ok", or what refused it
	python3 - <<'PY' 2>&1
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.151.1", 19471), 10))
r = pcnative.call(f, "set", col="c", key="probe:0", value="v")
print("ok" if "error" not in r else str(r["error"])[:200])
PY
}
k=0; P0=; while [ $k -lt 75 ]; do
	P1=$(probe); [ "$P1" = ok ] && break
	[ -z "$P0" ] && P0=$P1
	sleep 0.2; k=$((k+1))
done
[ -n "$P0" ] && echo "  ..   node 1 refused a write after the fleet formed: $P0; took one after $k polls"
[ "$P1" = ok ] || bad "node 1 takes no write after 15 s: $P1"
# the rebuild's drops need a WITNESS - a peer up, and complete, 30 s
# (WITNESS_GRACE_MS: three sweeps) before node 3's absence began; nodes 1
# and 2 booted ~10 s before the cut in v0.7.3-rc5 (both CIs), so every drop
# was deferred and node 3 kept all 200 deleted keys.  The cut comes 32 s
# after the starts (S455: a rebuild with no witness drops nothing)
w=$((32 - $(date +%s) + T0 - 3)); [ $w -gt 0 ] && sleep $w
W=$(ops 1 set r 0 300)
[ "$W" = 300 ] || bad "node 1 took $W of the 300 writes"
sleep 3
[ "$(held 3 r 0 300)" = 300 ] || bad "node 3 holds $(held 3 r 0 300) of the 300 keys before the cut"

# 1. the cut - the r: keys were stored on node 3 3 s before it, its own w:
# keys 3 s after: a second either side of the boundary is rounding
echo "cut 127.0.151.1 127.0.151.2 mcast" > "$D/cut3"
sleep 3
D1=$(ops 1 del r 0 200)
W3=$(ops 3 set w 0 50)
sleep 32
: > "$D/cut3"
IN=$(sed -n 's/.*in \([0-9]*\).*/\1/p' "$D/cutlog3" 2>/dev/null)
[ "${IN:-0}" -gt 0 ] 2>/dev/null && [ "$D1" = 200 ] &&
	ok "1. node 3 cut off 35 s ($IN datagrams consumed); node 1 deleted 200 keys meanwhile, node 3 accepted $W3 of 50 writes" ||
	bad "1. the cut: shim consumed ${IN:-0}, node 1 deleted $D1 of 200 - nothing below is a test"

# 2. healed: asked, and rebuilt
k=0; while [ $k -lt 150 ] && [ "$(st 3 'c["acks"]["rebuilds"]')" = 0 ]; do sleep 0.2; k=$((k+1)); done
sleep 2
RS=$(( $(st 1 'c["acks"]["rebuild_sent"]') + $(st 2 'c["acks"]["rebuild_sent"]') ))
RB=$(st 3 'c["acks"]["rebuilds"]')
TK=$(grep -h -c "took the rebuild asked of it" "$D/n1.log" "$D/n2.log" | awk '{ t += $1 } END { print t }')
[ "$RS" -ge 1 ] 2>/dev/null && [ "${RB:-0}" -ge 1 ] 2>/dev/null && [ "${RB:-0}" -le 2 ] && [ "${TK:-0}" -ge 1 ] &&
	ok "2. healed: node 3 was asked to rebuild ($RS ask(s)), ran $RB pass(es), and $TK asker(s) saw it taken" ||
	bad "2. after the heal: asks $RS, node 3's rebuilds ${RB:-?} (want 1 or 2), taken seen by ${TK:-0}; $(grep -h 'is back after\|asking it to rebuild\|says this node was away\|did not take' "$D"/n*.log | head -4 | tr '\n' ' ' | cut -c1-400)"

# 3 and 4: what node 3 holds once its pass is done
k=0; while [ $k -lt 150 ] && [ "$(held 3 r 0 200)" != 0 ]; do sleep 0.2; k=$((k+1)); done
sleep 2
G=$(held 3 r 0 200); L=$(held 3 r 200 300); O=$(held 3 w 0 50)
[ "$G" = 0 ] && [ "$L" = 100 ] &&
	ok "3. node 3 holds none of the 200 deleted keys and all 100 live ones" ||
	bad "3. node 3 holds $G of the 200 deleted (want 0) and $L of the 100 live (want 100); its reconcile: deferred $(st 3 'c.get("reconcile_deferred")') (a drop with no live witness), witnesses live $(st 3 'c.get("reconcile_witnesses_live")'), reconciled $(st 3 'c.get("reconciled")')"
[ "$O" = "$W3" ] && [ "${W3:-0}" -gt 0 ] &&
	ok "4. node 3 kept every write it accepted while cut off ($O of $W3)" ||
	bad "4. node 3 holds $O of the ${W3:-0} writes it accepted while cut off (want all, and more than 0)"

# 5. nothing node 3 kept came back to the others
G1=$(held 1 r 0 200); G2=$(held 2 r 0 200)
[ "$G1" = 0 ] && [ "$G2" = 0 ] &&
	ok "5. nodes 1 and 2 hold none of the 200 deleted keys after the heal" ||
	bad "5. after the heal node 1 holds $G1 and node 2 holds $G2 of the 200 deleted keys (want 0 0)"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombrebuildtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
