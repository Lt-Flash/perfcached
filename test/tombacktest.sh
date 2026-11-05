#!/bin/sh
# tombacktest.sh - 12ny AT-3b: every member counts, durably, which of each
# origin's numbered deletes it holds (clack.h, ackstate.h).
#
# A peer's delete never reached this node's WAL (an eager replica persists
# only what it authored), so "confirmed once the WAL has synced it" had no
# record to wait for.  Now a numbered delete - this node's own or a peer's -
# is logged as PC_WAL_T_DEL_N with its numbering, counted, and confirmed
# once the WAL has synced it; a snapshot carries the marks in `acks` beside
# it, and a restart rebuilds them from that file and the replay.  Three
# eager nodes; node 2 takes snapshots (a clean stop writes one), nodes 1 and
# 3 run on the WAL alone:
#   1. 60 deletes through node 1: node 1 confirms 60 in its own space,
#      nodes 2 and 3 confirm 60 in node 1's
#   2. node 3 killed (-9): its WAL holds 60 numbered deletes, 1..60, of
#      node 1's space; restarted, the replay counts them back - confirmed 60
#   3. node 2 stopped cleanly: its snapshot leaves `acks`; restarted, the
#      file restores the space - confirmed 60
#   4. 20 more: both restarted members count on past what they rebuilt (80)
#   5. node 1 restarted: a new incarnation is a new space - 5 deletes in it,
#      the old one kept at 80, on node 2 and on node 1 itself
#   6. AT-4: the keepalives carry the marks - node 2 holds what nodes 1 and 3
#      announce of both spaces, and every node's gate is open
#   7. AT-10: node 3 restarted as a build without the marks
#      (PERFCACHED_TEST_NO_ACKS): node 1's gate closes and names it, its
#      marks are cleared - an origin forgets nothing by marks
#   8. node 3 back as itself: the gate opens, it announces what it rebuilt
# FAIL-FIRST: before AT-3b /stats has no cluster.acks and the WAL no del-n;
# before AT-4 no node holds another's marks.
# Usage: test/tombacktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctack.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19411 19412 19413; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombacktest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	if [ $i = 2 ]; then SAVE="save = 900 1"; else SAVE="save = off"; fi
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = ta-client-secret
cluster = ta-cluster-secret
[listen]
tcp = 127.0.147.$i:1941$i
plaintext = loopback
[cluster]
multicast = 239.255.77.163:17283
advertise = 127.0.147.$i
mode = eager
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
$SAVE
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
done
start() { # start <i> [VAR=value]: the variable for this start only
	: > "$D/n$1.log"
	env ${2:-} "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & PIDS="$PIDS $!"; eval "P$1=$!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
for i in 1 2 3; do start $i; done
sleep 4
ak() { # ak <i> <python expr over a = /stats cluster.acks>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.147.%d" % i, 19410 + i), 10))
a = pcnative.call(f, "stats")["result"]["cluster"].get("acks")
print(eval(sys.argv[2]))
PY
}
ops() { # ops <verb> <from> <to>: through node 1, one call each
	python3 - "$1" "$2" "$3" <<'PY'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.147.1", 19411), 10))
for i in range(int(sys.argv[2]), int(sys.argv[3])):
    if sys.argv[1] == "set":
        pcnative.call(f, "set", col="c", key="g:%d" % i, value="v")
    else:
        pcnative.call(f, "del", col="c", key="g:%d" % i)
PY
}
# space <i> <origin> <incarn> <field>: that field of the space on node i, or -1
space() {
	ak "$1" "next((m['$4'] for m in a['marks'] if m['origin'] == '$2' and m['incarn'] == $3), -1)"
}
peers1() { # peers1: up to 15 s for node 1 to hold both peers - a delete sent
	# while one is still being admitted is a hole nothing fills before AT-6
	k=0; while [ $k -lt 75 ]; do
		[ "$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.147.1", 19411), 10))
print(pcnative.call(f, "stats")["result"]["cluster"].get("peers_up"))
PY
)" = 2 ] && { python3 "$T/waitready.py" 127.0.147.1:19411 127.0.147.2:19412 127.0.147.3:19413 || bad "a node is not ready"; return 0; }
		sleep 0.2; k=$((k+1))
	done
	bad "node 1 does not hold both peers"; return 1
}
waitfor() { # waitfor <i> <origin> <incarn> <confirmed>: up to 10 s
	k=0; while [ $k -lt 50 ] && [ "$(space "$1" "$2" "$3" confirmed)" != "$4" ]; do sleep 0.2; k=$((k+1)); done
	[ "$(space "$1" "$2" "$3" confirmed)" = "$4" ]
}

peers1
ops set 0 100
sleep 2
ops del 0 60
# 1. node 1's own space: the only one it has
O1=$(ak 1 "a['marks'][0]['origin'] if a and a['marks'] else ''")
I1=$(ak 1 "a['marks'][0]['incarn'] if a and a['marks'] else 0")
if [ -z "$O1" ]; then
	bad "1. node 1 reports no watermarks: $(ak 1 'a')"
else
	waitfor 1 "$O1" "$I1" 60 && waitfor 2 "$O1" "$I1" 60 && waitfor 3 "$O1" "$I1" 60 &&
		ok "1. 60 deletes through node 1: confirmed 60 in its space ($O1/$I1) on nodes 1, 2 and 3" ||
		bad "1. confirmed in $O1/$I1: node 1 $(space 1 "$O1" "$I1" confirmed), node 2 $(space 2 "$O1" "$I1" confirmed), node 3 $(space 3 "$O1" "$I1" confirmed) (want 60; node 2: $(ak 2 'a'))"
	[ "$(ak 2 'a["durable_on_apply"]')" = False ] && ok "1. fsync = everysec: confirmed when synced, not on apply" ||
		bad "1. node 2 durable_on_apply = $(ak 2 'a["durable_on_apply"]')"
fi

# 2. node 3: killed, its WAL read, restarted on the WAL alone
kill -9 $P3; wait $P3 2>/dev/null
"$BIN" -W "$D/w3" > "$D/w3.dump" 2>&1
N=$(grep " del-n " "$D/w3.dump" | grep -c "origin=$O1 incarn=$I1 ")
U=$(grep " del-n " "$D/w3.dump" | grep "origin=$O1 incarn=$I1 " | sed 's/.* num=\([0-9]*\).*/\1/' | sort -un | awk 'NR == 1 { lo = $1 } { hi = $1; n++ } END { printf "%d %d %d", n, lo, hi }')
[ "$N" = 60 ] && [ "$U" = "60 1 60" ] &&
	ok "2. node 3's WAL holds node 1's 60 numbered deletes, 1..60" ||
	bad "2. node 3's WAL: $N del-n records of $O1/$I1, distinct/low/high '$U' (want 60, '60 1 60'); first: $(grep -m1 ' del-n ' "$D/w3.dump")"
start 3
sleep 2
grep -q "60 numbered delete(s) counted from the WAL" "$D/n3.log" && [ "$(space 3 "$O1" "$I1" confirmed)" = 60 ] &&
	ok "2. restarted: the replay counted them back - confirmed 60" ||
	bad "2. node 3 restarted: confirmed $(space 3 "$O1" "$I1" confirmed); $(grep -m1 'recover: watermarks' "$D/n3.log")"

# 3. node 2: a clean stop, its snapshot and the file beside it
kill -TERM $P2; wait $P2 2>/dev/null
[ -s "$D/w2/acks" ] && ok "3. node 2 stopped cleanly: its snapshot left acks ($(wc -c < "$D/w2/acks") bytes)" ||
	bad "3. no acks beside node 2's snapshot: $(ls "$D/w2")"
start 2
sleep 2
grep -qE "[1-9][0-9]* numbering space\(s\) restored from the snapshot's" "$D/n2.log" && [ "$(space 2 "$O1" "$I1" confirmed)" = 60 ] &&
	ok "3. restarted: the file restored the space - confirmed 60 ($(grep -m1 'recover: watermarks' "$D/n2.log" | sed 's/.*watermarks - //'))" ||
	bad "3. node 2 restarted: confirmed $(space 2 "$O1" "$I1" confirmed); $(grep -m1 'recover: watermarks' "$D/n2.log")"

# 4. counting on past what was rebuilt
peers1
sleep 2
ops del 60 80
waitfor 2 "$O1" "$I1" 80 && waitfor 3 "$O1" "$I1" 80 &&
	ok "4. 20 more: both restarted members count on - confirmed 80" ||
	bad "4. after 20 more: node 2 $(space 2 "$O1" "$I1" confirmed), node 3 $(space 3 "$O1" "$I1" confirmed) (want 80)"

# 5. node 1 restarted: a new incarnation, a new space
kill -TERM $P1; wait $P1 2>/dev/null
start 1
peers1
sleep 2
ops del 80 85
I1b=$(ak 1 "next((m['incarn'] for m in a['marks'] if m['origin'] == '$O1' and m['incarn'] != $I1), 0)")
[ "$I1b" != 0 ] && waitfor 2 "$O1" "$I1b" 5 && waitfor 1 "$O1" "$I1b" 5 &&
	[ "$(space 2 "$O1" "$I1" confirmed)" = 80 ] &&
	[ "$(space 1 "$O1" "$I1" confirmed)" = 80 ] && [ "$(space 1 "$O1" "$I1b" confirmed)" = 5 ] &&
	ok "5. node 1 restarted: its new space $O1/$I1b holds 5, the old one 80 - on node 2 and on node 1" ||
	bad "5. node 1 restarted (incarnation '$I1b'): node 2 old $(space 2 "$O1" "$I1" confirmed) new $(space 2 "$O1" "$I1b" confirmed), node 1 old $(space 1 "$O1" "$I1" confirmed) new $(space 1 "$O1" "$I1b" confirmed) (want 80, 5)"

# 6. what node 2 holds of the others' announcements: the members reporting
# a space at a mark (node 2 keeps no entry for itself)
rep() { # rep <i> <origin> <incarn> <mark>: how many members node i heard say it
	ak "$1" "sum(1 for m in a['fleet']['marks'] if m['origin'] == '$2' and m['incarn'] == $3 and m['confirmed'] == $4)"
}
k=0; while [ $k -lt 50 ] && { [ "$(rep 2 "$O1" "$I1" 80)" != 2 ] || [ "$(rep 2 "$O1" "$I1b" 5)" != 2 ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(rep 2 "$O1" "$I1" 80)" = 2 ] && [ "$(rep 2 "$O1" "$I1b" 5)" = 2 ] &&
	ok "6. AT-4: node 2 heard nodes 1 and 3 announce both of node 1's spaces (80 and 5)" ||
	bad "6. node 2 holds $(rep 2 "$O1" "$I1" 80) report(s) of the old space at 80, $(rep 2 "$O1" "$I1b" 5) of the new at 5 (want 2 each): $(ak 2 'a["fleet"]')"
G=$(for i in 1 2 3; do ak $i 'a["fleet"]["confirming"]'; done | tr '\n' ' ')
[ "$G" = "True True True " ] && ok "6. AT-10: every member announces its marks - the gate is open on all three" ||
	bad "6. the gate on nodes 1/2/3: $G"

# 7. node 3 as a build without the marks
N3=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.147.3", 19413), 10))
print(pcnative.call(f, "stats")["result"]["cluster"]["node"])
PY
)
M3=$(ak 2 "next((m['member'] for m in a['fleet']['marks'] if m['member'] != '$O1'), '')")
kill -TERM $P3; wait $P3 2>/dev/null
start 3 PERFCACHED_TEST_NO_ACKS=1
peers1
k=0; while [ $k -lt 50 ] && [ "$(ak 1 'a["fleet"]["confirming"]')" != False ]; do sleep 0.2; k=$((k+1)); done
W=$(ak 1 'a["fleet"]["waiting_for"]')
H=$(ak 1 "sum(1 for m in a['fleet']['marks'] if m['member'] == '$M3')")
[ "$(ak 1 'a["fleet"]["confirming"]')" = False ] && [ "$W" = "[$N3]" ] && [ "$H" = 0 ] &&
	ok "7. AT-10: node 3 announces no marks - node 1's gate closes and names it ($W), its marks cleared" ||
	bad "7. node 3 without marks: node 1 confirming $(ak 1 'a["fleet"]["confirming"]'), waiting_for $W (node 3 is $N3), node 3's marks held $H"

# 8. node 3 as itself again
kill -TERM $P3; wait $P3 2>/dev/null
start 3
peers1
k=0; while [ $k -lt 50 ] && { [ "$(ak 1 'a["fleet"]["confirming"]')" != True ] || [ "$(ak 1 "sum(1 for m in a['fleet']['marks'] if m['member'] == '$M3' and m['origin'] == '$O1' and m['incarn'] == $I1 and m['confirmed'] == 80)")" != 1 ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(ak 1 'a["fleet"]["confirming"]')" = True ] &&
	[ "$(ak 1 "sum(1 for m in a['fleet']['marks'] if m['member'] == '$M3' and m['origin'] == '$O1' and m['incarn'] == $I1 and m['confirmed'] == 80)")" = 1 ] &&
	ok "8. node 3 back as itself: the gate opens, and it announces the 80 it rebuilt" ||
	bad "8. node 3 back: node 1 confirming $(ak 1 'a["fleet"]["confirming"]'), node 3's report of the old space: $(ak 1 "[m for m in a['fleet']['marks'] if m['member'] == '$M3']")"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombacktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
