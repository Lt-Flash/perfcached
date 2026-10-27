#!/bin/sh
# tombresynctest.sh - S455: a member the fleet cannot vouch for record by
# record resyncs - drops what it stored before its absence and pulls the
# fleet's state - instead of keeping (and spreading) keys deleted while it
# was away.
#
# A returning eager member's records are ruled on by a WITNESS: a peer up,
# and complete, 30 s before the absence began (WITNESS_GRACE_MS).  With
# none, S223 skips the reconcile and keeps everything - the safe side when
# the fleet may be incomplete, and wrong for a member back after a long
# absence while the fleet lived on (rolled to a new build meanwhile).
# Four arms, four fresh eager fleets:
#   A. a RESTART after an absence past resync_after_s (20 s) during which
#      the fleet rolled - no witness at all: node 3's backup, 200 deletes
#      confirmed and forgotten, node 3 stopped, nodes 1 and 2 rolled and
#      serving 30 s and more, node 3 restored from the backup - it
#      resyncs: none of the 200 anywhere, all 100 kept on it
#   B. a SPLIT with no witness (cut ~10 s after the starts, the peers too
#      young to witness it): asked to rebuild, it resyncs - none of the 200
#      deleted, all 100 kept, and the 50 it wrote while cut off kept
#   C. AWAY LONGER than resync_after_s (5 s), witnesses present: stopped
#      cleanly 8 s, it resyncs anyway and holds the fleet's 100 after
#   D. a RESTART with no witness INSIDE resync_after_s still skips (S223):
#      nothing dropped, no resync - the safe side when boot times cannot
#      tell a rolled fleet from an incomplete survivor (outagetest)
# FAIL-FIRST: before S455 arm A's node 3 skipped its reconcile and held the
# 200 (nodes 1 and 2 got them back - rc8's tombrestoretest); arm B's
# rebuild skipped and node 3 kept the 200; arm C reconciled by the
# witness, no resync.
# Usage: test/tombresynctest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctrsy.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombresynctest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19611 19612 19613; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombresynctest: port $p already bound" >&2; exit 1; }
done
A=                                     # the arm: its dir and its secret
mkconf() { # mkconf <arm> <extra [cluster] line>...
	A=$1; shift
	mkdir -p "$D/$A"
	for i in 1 2 3; do
		mkdir -p "$D/$A/w$i" "$D/$A/s$i"
		if [ $i = 3 ]; then SAVE="save = 900 1"; else SAVE="save = off"; fi
		{
			cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/$A/s$i
[memory]
arena_mb = 64
[secrets]
client = rs$A-client-secret
cluster = rs$A-cluster-secret
[listen]
tcp = 127.0.156.$i:1961$i
plaintext = loopback
[cluster]
multicast = 239.255.77.177:17297
advertise = 127.0.156.$i
mode = eager
collections = c
C
			for l in "$@"; do echo "$l"; done
			cat <<C
[wal]
dir = $D/$A/w$i
segment_mb = 8
probe = no
$SAVE
[collection c]
buckets_log2 = 12
C
		} > "$D/$A/n$i.conf"
		chmod 600 "$D/$A/n$i.conf"
	done
	: > "$D/$A/cut3"
}
start() { # start <i> [shim]
	if [ -n "${2:-}" ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/$A/cut3" PC_NETCUT_LOG="$D/$A/cutlog3" \
			"$BIN" -f "$D/$A/n$1.conf" >> "$D/$A/n$1.log" 2>&1 &
	else
		"$BIN" -f "$D/$A/n$1.conf" >> "$D/$A/n$1.log" 2>&1 &
	fi
	PIDS="$PIDS $!"; eval "P$1=$!"
	k=0; while [ $k -lt 150 ] && [ "$(grep -c "perfcached ready" "$D/$A/n$1.log")" -lt "$(( $(eval echo \${S$1:-0}) + 1 ))" ]; do sleep 0.1; k=$((k+1)); done
	eval "S$1=\$(( \${S$1:-0} + 1 ))"
}
stopn() { eval "kill -TERM \$P$1; wait \$P$1 2>/dev/null"; }
stopall() { for i in 1 2 3; do eval "[ -n \"\${P$i:-}\" ] && kill -TERM \$P$i 2>/dev/null; wait \$P$i 2>/dev/null"; eval "P$i= S$i=0"; done; }
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.156.%d" % i, 19610 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <node> <set|del> <prefix> <from> <to>: prints how many succeeded
	python3 - "$@" <<'PY'
import socket, sys, pcnative
n, op, pre, a, b = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
f = pcnative.wrap(socket.create_connection(("127.0.156.%d" % n, 19610 + n), 10))
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
f = pcnative.wrap(socket.create_connection(("127.0.156.%d" % n, 19610 + n), 10))
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key="%s:%d" % (pre, i)).get("result") or {}).get("found")))
PY
}
ready() { # ready <i>...: every one READY (15 s)
	a=""; for i in "$@"; do a="$a 127.0.156.$i:1961$i"; done
	python3 "$T/waitready.py" $a
}
gate() { # 2 peers up on node 1 and the acks gate open
	k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
	ready 1 2 3 || bad "$A: a node is not ready"
}
resynced() { # resynced <i>: wait (40 s) for a resync to finish there
	k=0; while [ $k -lt 200 ] && { [ "$(st "$1" 'c.get("resyncs")')" = 0 ] || [ "$(st "$1" 'c.get("resyncing")')" != False ]; }; do sleep 0.2; k=$((k+1)); done
	ready "$1" > /dev/null
}

# ---- A. a restart past resync_after_s, the fleet rolled meanwhile ---
mkconf A "resync_after_s = 20"
for i in 1 2 3; do start $i; done
gate
ops 1 set k 0 300 > /dev/null; sleep 2
stopn 3
cp -a "$D/A/s3" "$D/A/s3.bak" && cp -a "$D/A/w3" "$D/A/w3.bak"
start 3; gate
ops 1 del k 0 200 > /dev/null
k=0; while [ $k -lt 100 ] && [ "$(st 1 'c["tombstones_retained"]["numbered"]')" != 0 ]; do sleep 0.2; k=$((k+1)); done
stopn 3
# the fleet lives on: nodes 1 and 2 rolled, then serving 32 s
stopn 1; start 1; ready 1 2 > /dev/null; sleep 2
stopn 2; start 2; ready 1 2 > /dev/null
R0=$(date +%s)
G1=$(held 1 k 0 200); L1=$(held 1 k 200 300)
[ "$G1 $L1" = "0 100" ] || bad "A. before node 3's return node 1 holds $G1 of the 200 deleted and $L1 of the 100 kept (want 0 100) - nothing below is a test"
w=$((33 - $(date +%s) + R0)); [ $w -gt 0 ] && sleep $w
rm -rf "$D/A/s3" "$D/A/w3"; cp -a "$D/A/s3.bak" "$D/A/s3"; cp -a "$D/A/w3.bak" "$D/A/w3"
start 3
resynced 3
RS=$(st 3 'c.get("resyncs")'); RD=$(st 3 'c.get("resync_dropped")')
G3=$(held 3 k 0 200); L3=$(held 3 k 200 300); G1=$(held 1 k 0 200); G2=$(held 2 k 0 200)
[ "${RS:-0}" -ge 1 ] 2>/dev/null && grep -q "resync - away ~[0-9]* s, longer than resync_after_s (20)" "$D/A/n3.log" &&
	ok "A. restored after ~45 s away (past resync_after_s 20), the fleet rolled meanwhile and no witness: node 3 resynced ($RS, $RD record(s) dropped)" ||
	bad "A. node 3 restored from its backup: resyncs ${RS:-?} (want 1 or more); $(grep -m2 -h 'resync -\|reconcile SKIPPED\|reconcile -' "$D/A/n3.log" | tr '\n' ' ' | cut -c1-300)"
[ "$G3 $L3 $G1 $G2" = "0 100 0 0" ] &&
	ok "A. after it: node 3 holds none of the 200 deleted and all 100 kept; nodes 1 and 2 hold none of the 200" ||
	bad "A. node 3 holds $G3 of the 200 deleted (want 0) and $L3 of the 100 kept (want 100); nodes 1/2 hold $G1/$G2 of the 200 (want 0 0)"
stopall

# ---- B. a partition, no witness --------------------------------------
mkconf B "tombstone_retain_s = 20"
start 1; start 2; start 3 shim
gate
ops 1 set r 0 300 > /dev/null; sleep 3
echo "cut 127.0.156.1 127.0.156.2 mcast" > "$D/B/cut3"
sleep 3
D1=$(ops 1 del r 0 200); W3=$(ops 3 set w 0 50)
sleep 32
: > "$D/B/cut3"
resynced 3
sleep 2
RS=$(st 3 'c.get("resyncs")')
G3=$(held 3 r 0 200); L3=$(held 3 r 200 300); O3=$(held 3 w 0 "$W3"); G1=$(held 1 r 0 200); G2=$(held 2 r 0 200)
[ "${RS:-0}" -ge 1 ] 2>/dev/null && [ "$D1" = 200 ] && grep -q "resync - no peer was up long enough before this absence" "$D/B/n3.log" &&
	ok "B. cut off ~10 s after the starts, no witness: asked to rebuild, node 3 resynced ($RS)" ||
	bad "B. after the heal: resyncs ${RS:-?} (want 1 or more), deletes through node 1 $D1 of 200; $(grep -m2 -h 'resync -\|asked to rebuild\|says this node was away' "$D/B/n3.log" | tr '\n' ' ' | cut -c1-300)"
[ "$G3 $L3 $O3 $G1 $G2" = "0 100 $W3 0 0" ] && [ "${W3:-0}" -gt 0 ] 2>/dev/null &&
	ok "B. node 3 holds none of the 200 deleted, all 100 kept and the $W3 it wrote while cut off; nodes 1 and 2 hold none of the 200" ||
	bad "B. node 3 holds $G3 of the 200 deleted (want 0), $L3 of the 100 kept (want 100), $O3 of its $W3 own writes (want all); nodes 1/2 hold $G1/$G2 of the 200 (want 0 0)"
stopall

# ---- C. away longer than resync_after_s, witnesses present -----------
mkconf C "resync_after_s = 5"
for i in 1 2 3; do start $i; done
C0=$(date +%s)
gate
ops 1 set c 0 100 > /dev/null
w=$((33 - $(date +%s) + C0)); [ $w -gt 0 ] && sleep $w
stopn 3; sleep 8
start 3
resynced 3
RS=$(st 3 'c.get("resyncs")'); L3=$(held 3 c 0 100)
[ "${RS:-0}" -ge 1 ] 2>/dev/null && grep -q "resync - away ~[0-9]* s, longer than resync_after_s (5)" "$D/C/n3.log" && [ "$L3" = 100 ] &&
	ok "C. away ~8 s past resync_after_s 5 with witnesses up: node 3 resynced ($RS) and holds the fleet's 100" ||
	bad "C. resyncs ${RS:-?} (want 1 or more), node 3 holds $L3 of 100; $(grep -m2 -h 'resync -\|reconcile -' "$D/C/n3.log" | tr '\n' ' ' | cut -c1-300)"
stopall

# ---- D. a restart, no witness, inside resync_after_s: S223's skip ----
mkconf D
for i in 1 2 3; do start $i; done
gate
ops 1 set d 0 100 > /dev/null; sleep 2
stopn 3; sleep 3
start 3
k=0; while [ $k -lt 75 ] && ! grep -q "reconcile SKIPPED\|reconcile - \|resync -" "$D/D/n3.log"; do sleep 0.2; k=$((k+1)); done
ready 3 > /dev/null
RS=$(st 3 'c.get("resyncs")'); SK=$(st 3 'c.get("reconcile_skipped")'); L3=$(held 3 d 0 100)
[ "$RS" = 0 ] && [ "$SK" = True ] && [ "$L3" = 100 ] &&
	ok "D. restarted 3 s after a stop, peers ~10 s old (no witness), inside resync_after_s: the reconcile was skipped, no resync, all 100 kept" ||
	bad "D. resyncs ${RS:-?} (want 0), reconcile_skipped ${SK:-?} (want True), node 3 holds $L3 of 100; $(grep -m2 -h 'resync -\|reconcile SKIPPED\|reconcile -' "$D/D/n3.log" | tr '\n' ' ' | cut -c1-300)"
stopall

for a in A B C D; do
	for i in 1 2 3; do
		grep -q "CRIT" "$D/$a/n$i.log" 2>/dev/null &&
			bad "$a: node $i logged CRIT: $(grep -m2 'CRIT' "$D/$a/n$i.log" | tr '\n' ' ' | cut -c1-220)"
	done
done
echo "tombresynctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
