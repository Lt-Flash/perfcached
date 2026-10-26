#!/bin/sh
# tombgaptest.sh - S427 (0.7.1; PROD 2026-10-20): cluster.peer_absent says a
# member's absence was NOT covered by the retained deletes only when it was
# not.  0.6.6 raised it on ANY early drop by the cap while the member was
# away - with the cap biting the OLDEST deletes go, ones made long before the
# absence - and every 34-s restart on PROD said "back after 34 s, past
# tombstone_retain_s 900 s", which it was not.
#
# Three eager nodes, tombstone_retain_s 120, tombstone_retain_max 1000 (its
# minimum), and a steady 50 deletes a second through node 1: the cap bites all the time, and
# the table covers the last ~20 s (1000 / 50).  Node 3 is FROZEN (SIGSTOP) -
# silent to its peers, then back (SIGCONT), no restart:
#   A. frozen 8 s - a gap (~18 s) inside the coverage C (measured: the oldest
#      delete held): node 1 notes the silence and raises NO peer_absent
#   B. frozen C + 5 s - a gap past C: cluster.peer_absent, naming the cap
#      ("tombstone_retain_max 1000 dropped deletes made while it was away"),
#      not the 120-s window it was well inside
# Fail-first: before S427, A raises "past tombstone_retain_s 120 s".
# Usage: test/tombgaptest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctgap.XXXXXX)
PIDS="" DP=""
trap 'kill $DP 2>/dev/null; for p in $PIDS; do kill -CONT $p 2>/dev/null; grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 17971 17972 17973; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombgaptest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60
for i in 1 2 3; do
	mkdir -p "$D/w$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = tg-client-secret
cluster = tg-cluster-secret
[listen]
tcp = 127.0.51.$i:1797$i
plaintext = loopback
[cluster]
multicast = 239.255.77.157:17277
advertise = 127.0.51.$i
mode = eager
tombstone_retain_s = 120
tombstone_retain_max = 1000
# 12ny AT-5: this suite tests the time rule's cap - no forgetting by marks
tombstone_acks = no
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	mkdir -p "$D/s$i"; chmod 600 "$D/n$i.conf"
	"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 & PIDS="$PIDS $!"; eval "P$i=$!"
done
for i in 1 2 3; do
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log")"; exit 1; }
done
sleep 4
al() { # al <python expr over a = node 1's alerts block>
	python3 - "$1" <<'PY' 2>/dev/null
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.51.1", 17971), 10))
a = pcnative.call(f, "stats")["result"]["alerts"]
print(eval(sys.argv[1]))
PY
}
# the driver: a set and its delete, 20 a second, through node 1 - for the whole suite
python3 - <<'PY' > "$D/driver.log" 2>&1 &
import socket, time, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.51.1", 17971), 10))
i = 0
while True:
    t = time.time()
    pcnative.call(f, "set", col="c", key="g:%d" % i, value="v")
    pcnative.call(f, "del", col="c", key="g:%d" % i)
    i += 1
    time.sleep(max(0, 1 / 20.0 - (time.time() - t)))
PY
DP=$!
sleep 60                              # the cap is biting: 1000 kept, the rest dropped early
r=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.51.1", 17971), 10))
t = pcnative.call(f, "stats")["result"]["cluster"]["tombstones_retained"]
print(t["entries"], t["dropped_early"], t["oldest_age_s"])
PY
)
set -- $r
C=${3:-0}                             # the table's coverage: the oldest delete it still holds
[ "${2:-0}" -gt 0 ] 2>/dev/null && ok "the cap bites: node 1 keeps $1 deletes, the oldest ${C} s old (the coverage), dropped early $2" || bad "the cap is not biting: $r"
pa() { al 'sum(1 for x in a["recent"] if x["key"] == "cluster.peer_absent")'; }
silent() { # silent <seconds>: node 3 frozen, then back; waits until node 1 notes its return
	n0=$(grep -c "is back after" "$D/n1.log")
	kill -STOP $P3; sleep "$1"; kill -CONT $P3
	k=0; while [ $k -lt 200 ] && [ "$(grep -c "is back after" "$D/n1.log")" = "$n0" ]; do sleep 0.1; k=$((k+1)); done
	sleep 3                           # the alert tick
}
gap() { grep "is back after" "$D/n1.log" | tail -1 | sed -n 's/.*is back after \([0-9]*\) s.*/\1/p'; }
# the gap node 1 measures runs from the silence to the return - ~10 s more
# than the freeze (detection, the purge at 6 s, readmission), and a freeze
# shorter than the detection is no gap at all: A freezes 8 s (a gap of ~18),
# B past the coverage C; the verdicts are on the gap node 1 logged
c0=$(pa)
silent 8
g=$(gap); c1=$(pa)
if [ -n "$g" ] && [ "$g" -lt "$C" ] 2>/dev/null; then
	[ "$c1" = "$c0" ] && ok "A. a gap of $g s, inside the $C s the table covers: no cluster.peer_absent" ||
		bad "A. a $g-s gap inside the $C-s coverage raised: $(al '[x["text"][:140] for x in a["recent"] if x["key"] == "cluster.peer_absent"][:1]')"
else
	bad "A. the short silence did not give a gap under the coverage (gap '$g', coverage $C) - the rig cannot test it"
fi
silent $((C + 5))
c2=$(pa); t=$(al '[x["text"] for x in a["recent"] if x["key"] == "cluster.peer_absent"][:1]'); g=$(gap)
[ "${g:-0}" -gt "$C" ] 2>/dev/null || bad "B. the long silence gave a gap of '$g' s, not past the $C-s coverage"
case "$t" in
*"tombstone_retain_max 1000"*"dropped deletes made while it was away"*)
	[ "$c2" -gt "$c1" ] 2>/dev/null && ok "B. a gap of $g s, past the $C-s coverage: cluster.peer_absent names the cap - $t" || bad "B. the text is right but no new event ($c1 -> $c2)" ;;
*) bad "B. a $g-s gap past the $C-s coverage: peer_absent $c1 -> $c2, text $t" ;;
esac
case "$t" in *"past tombstone_retain_s"*) bad "B. it claims the 120-s window was passed: $t" ;; esac
kill $DP 2>/dev/null; DP=
echo "tombgaptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
