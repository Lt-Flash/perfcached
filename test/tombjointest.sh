#!/bin/sh
# tombjointest.sh - 12ny AT-7: a node that joins takes its floors from the
# nodes it bootstraps from, so the prefixes forgotten before it was a member
# are never asked for, and it confirms like any member.
#
# Before: a joiner never held the start of a space whose deletes the fleet
# had already forgotten.  Its mark for that space stayed 0, the lowest among
# the members with it, and from the day it joined nothing of the space was
# forgotten again (the time rule only); with AT-6 it also asked for the
# forgotten prefix every second.  Now a node that came back with nothing
# floors each space at the lowest mark its bootstrap's candidates announced
# - every delete they had confirmed is already reflected in what they send.
# Nodes 1 and 2, then node 3 joins empty:
#   1. 200 deletes through node 1 are forgotten by nodes 1 and 2
#   2. node 3 joins: it floors node 1's space (acks.floors), confirming it
#      at 200 or more without a single delete of it applied
#   3. 100 more deletes: node 1 forgets them all, node 3 counted
#   4. node 3 holds none of the 300 deleted keys, and it asked for no more
#      than a handful of holes
# FAIL-FIRST: before AT-7 node 3's mark for the space is 0 - node 1 keeps
# the 100, and node 3 asks for [1, 200] over and over.
# Usage: test/tombjointest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctjoin.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19451 19452 19453; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombjointest: port $p already bound" >&2; exit 1; }
done
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
client = tj-client-secret
cluster = tj-cluster-secret
[listen]
tcp = 127.0.150.$i:1945$i
plaintext = loopback
[cluster]
multicast = 239.255.77.168:17288
advertise = 127.0.150.$i
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
done
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 & PIDS="$PIDS $!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.150.%d" % i, 19450 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <set|del> <from> <to> - a refused call is said, not dropped
	python3 - "$1" "$2" "$3" <<'PY'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.150.1", 19451), 10))
bad = []
for i in range(int(sys.argv[2]), int(sys.argv[3])):
    if sys.argv[1] == "set":
        r = pcnative.call(f, "set", col="c", key="j:%d" % i, value="v")
    else:
        r = pcnative.call(f, "del", col="c", key="j:%d" % i)
    if "error" in r:
        bad.append(r["error"])
if bad:
    print("  ..   ops %s %s-%s: %d refused - the first: %s" % (sys.argv[1], sys.argv[2], sys.argv[3], len(bad), str(bad[0])[:200]))
PY
}
held1() { # held1 <from> <to> - how many of j:<from>..<to-1> node 1 answers
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.150.1", 19451), 10))
print(sum(1 for i in range(int(sys.argv[1]), int(sys.argv[2])) if (pcnative.call(f, "get", col="c", key="j:%d" % i).get("result") or {}).get("found")))
PY
}
probe() { # a write node 1 takes: "ok", or what refused it
	python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.150.1", 19451), 10))
r = pcnative.call(f, "set", col="c", key="j:probe", value="v")
print("ok" if "error" not in r else str(r["error"])[:200])
PY
}
start 1; start 2
k=0; while [ $k -lt 75 ] && { [ "$(st 1 'c.get("peers_up")')" != 1 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done

# writes taken before any are made: rc2 and rc3 (both CIs, every run) showed
# node 1 with not one delete applied, noted or numbered after the 200 below -
# the writes went unanswered-for while the fleet looked formed.  Wait for a
# write node 1 takes, and say what refused the others
k=0; P0=; while [ $k -lt 75 ]; do
	P1=$(probe); [ "$P1" = ok ] && break
	[ -z "$P0" ] && P0=$P1
	sleep 0.2; k=$((k+1))
done
[ -n "$P0" ] && echo "  ..   node 1 refused a write after the fleet formed: $P0; took one after $k polls"
[ "$P1" = ok ] || bad "node 1 takes no write after 15 s: $P1"

# 1. 200 deletes, forgotten; 50 keys kept so the bootstrap has a dataset
ops set 0 250; ops del 0 200
H1=$(held1 0 250)
[ "$H1" = 50 ] || bad "1. node 1 holds $H1 of the 250 written (want the 50 not deleted)"
k=0; while [ $k -lt 75 ] && [ "$(st 1 'c["tombstones_retained"]["numbered"]') $(st 2 'c["tombstones_retained"]["numbered"]')" != "0 0" ]; do sleep 0.2; k=$((k+1)); done
O1=$(st 1 "c['acks']['marks'][0]['origin']"); I1=$(st 1 "c['acks']['marks'][0]['incarn']")
# the premise: node 1 numbered them - its own space is in its marks.  rc2
# (both CIs) passed this step with no space at all, and step 2 had nothing
# to look for: say what node 1 held instead
[ -n "$O1" ] && [ -n "$I1" ] ||
	bad "1. node 1 shows no numbering space of its own: acks $(st 1 '{x: c["acks"][x] for x in ("spaces", "applied", "dups", "far", "full", "marks")}'); fleet $(st 1 'c["acks"]["fleet"]'); retained $(st 1 'c["tombstones_retained"]')"
[ "$(st 1 'c["tombstones_retained"]["numbered"]') $(st 2 'c["tombstones_retained"]["numbered"]')" = "0 0" ] &&
	ok "1. 200 deletes through node 1, forgotten by nodes 1 and 2 (space $O1/$I1)" ||
	bad "1. after 200 deletes: numbered $(st 1 'c["tombstones_retained"]["numbered"]') / $(st 2 'c["tombstones_retained"]["numbered"]') (want 0 0)"

# 2. node 3 joins empty
start 3
k=0; while [ $k -lt 100 ] && [ "$(st 3 'c.get("peers_up")')" != 2 ]; do sleep 0.2; k=$((k+1)); done
sleep 3
F3=$(st 3 'c["acks"]["floors"]')
M3=$(st 3 "next((m['confirmed'] for m in c['acks']['marks'] if m['origin'] == '$O1' and m['incarn'] == $I1), -1)")
[ "${F3:-0}" -ge 1 ] 2>/dev/null && [ "${M3:--1}" -ge 200 ] 2>/dev/null &&
	ok "2. node 3 joined empty: $F3 space(s) floored from its bootstrap's sources, node 1's confirmed at $M3" ||
	bad "2. node 3 joined: floors ${F3:-?}, node 1's space confirmed ${M3:-?} (want 200 or more): $(grep -m2 'floored\|bootstrap' "$D/n3.log" | tr '\n' ' '); node 3's marks $(st 3 'c["acks"]["marks"]'), what it heard announced $(st 3 'c["acks"]["fleet"]["marks"]')"

# 3. 100 more: forgotten with node 3 counted
k=0; while [ $k -lt 75 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
ops set 200 300; ops del 200 300
k=0; while [ $k -lt 75 ] && [ "$(st 1 'c["tombstones_retained"]["numbered"]')" != 0 ]; do sleep 0.2; k=$((k+1)); done
[ "$(st 1 'c["tombstones_retained"]["numbered"]')" = 0 ] &&
	ok "3. 100 more deletes: node 1 forgot them all, node 3 counted" ||
	bad "3. after 100 more: node 1 holds $(st 1 'c["tombstones_retained"]["numbered"]') numbered; node 3's marks: $(st 3 'c["acks"]["marks"]')"

# 4. node 3's keys, and its asking
L=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.150.3", 19453), 10))
print(sum(1 for i in range(300) if (pcnative.call(f, "get", col="c", key="j:%d" % i).get("result") or {}).get("found")))
PY
)
A3=$(st 3 'c["acks"]["gap_asked"]')
[ "$L" = 0 ] && [ "${A3:-99}" -le 10 ] 2>/dev/null &&
	ok "4. node 3 holds none of the 300 deleted keys, and asked for $A3 hole(s)" ||
	bad "4. node 3 holds ${L:-?} deleted keys, asked for ${A3:-?} hole(s) (want 0, and 10 or fewer)"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombjointest: $pass passed, $fail failed"
[ $fail -eq 0 ]
