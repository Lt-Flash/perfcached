#!/bin/sh
# tombforgettest.sh - 12ny AT-5 (AT-12 a, b): a numbered delete is forgotten
# once every member holds it, not after tombstone_retain_s.
#
# The retained-delete table kept every delete for the whole window whether
# or not any member could still need it - PROD held ~436,000 on 2026-10-24
# (DESIGN 12ny AT-0).  Now an origin forgets through the lowest mark the
# members confirm; a member gone silent keeps its marks where it left them,
# so what it has not confirmed stays, and comes to it when it is back.
# Three eager nodes, fsync everysec, tombstone_retain_s 900:
#   1. 40 set+delete pairs a second through node 1 for 20 s: the table on
#      every node stays at a few seconds' worth (under 400), the 800 deletes
#      are forgotten - the time rule would hold all 800
#   2. the driver stops: the tables empty
#   3. node 3 frozen (SIGSTOP) 10 s while the deletes go on - past the purge
#      (6 s), so it leaves node 1's peer table but stays a member: node 1
#      keeps every delete made meanwhile
#   4. node 3 back (SIGCONT): it is given what it missed (numbered, so it
#      counts it), confirms, and the tables shrink again - and node 3 holds
#      none of the deleted keys.  What it missed reached it by its own marks
#      (it asks past them, AT-7) or by node 1's catch-up (the deletes its
#      marks do not cover - v0.7.3-rc1)
#   5. AT-5 (c): 100 keys with a 2-s TTL and 100 immortal keys given a 2-s
#      TTL by EXPIRE (shortened - an older, immortal copy could outlive
#      them): the first hundred's expiries are let go after the hold, the
#      shortened hundred's stay, on every node
# FAIL-FIRST: before AT-5 step 1's table holds every delete (800).
# Usage: test/tombforgettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctfg.XXXXXX)
PIDS="" DP=""
trap 'kill $DP 2>/dev/null; for p in $PIDS; do kill -CONT $p 2>/dev/null; grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19421 19422 19423; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombforgettest: port $p already bound" >&2; exit 1; }
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
client = tf-client-secret
cluster = tf-cluster-secret
[listen]
tcp = 127.0.148.$i:1942$i
plaintext = loopback
[cluster]
multicast = 239.255.77.166:17286
advertise = 127.0.148.$i
mode = eager
collections = c
# one copy a peer: on the group (AT-9) a frozen member's socket keeps the
# deletes made while it is stopped, and it misses nothing for case 4 to see
data_multicast = off
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
	"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 & PIDS="$PIDS $!"; eval "P$i=$!"
done
for i in 1 2 3; do
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log" | tr '\n' ' ')"; exit 1; }
done
tr() { # tr <i> <python expr over t = cluster.tombstones_retained, c = cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.148.%d" % i, 19420 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
t = c["tombstones_retained"]
print(eval(sys.argv[2]))
PY
}
k=0; while [ $k -lt 75 ] && { [ "$(tr 1 'c.get("peers_up")')" != 2 ] || [ "$(tr 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(tr 1 'c["acks"]["fleet"]["confirming"]')" = True ] || { bad "the fleet does not confirm: $(tr 1 'c.get("acks")')"; exit 1; }

drive() { # drive <rate> <seconds> <first>: set+delete pairs through node 1
	python3 - "$1" "$2" "$3" <<'PY'
import socket, sys, time, pcnative
rate, secs, first = float(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.148.1", 19421), 10))
t0 = time.time(); i = first
while time.time() - t0 < secs:
    t = time.time()
    pcnative.call(f, "set", col="c", key="g:%d" % i, value="v")
    pcnative.call(f, "del", col="c", key="g:%d" % i)
    i += 1
    time.sleep(max(0, 1 / rate - (time.time() - t)))
print(i)
PY
}
# 1. a steady rate: the table stays at a few seconds' worth
drive 40 20 0 > "$D/d1" &
DP=$!
MX=0 k=0
while kill -0 $DP 2>/dev/null; do
	for i in 1 2 3; do
		e=$(tr $i 't["entries"]'); [ "${e:-0}" -gt "$MX" ] 2>/dev/null && MX=$e
	done
	sleep 1; k=$((k+1))
done
wait $DP; DP=
N1=$(cat "$D/d1")
F1=$(tr 1 't["forgotten"]')
[ "$MX" -lt 400 ] 2>/dev/null && [ "${F1:-0}" -ge $((N1 - 400)) ] 2>/dev/null &&
	ok "1. $N1 deletes at 40 a second: no node held more than $MX at a time, node 1 forgot $F1 (the time rule keeps all $N1)" ||
	bad "1. $N1 deletes: the most held was $MX (want under 400), node 1 forgot ${F1:-?}: $(tr 1 't')"

# 2. the driver stops: the tables empty
k=0; while [ $k -lt 50 ]; do
	E="$(tr 1 't["numbered"]') $(tr 2 't["numbered"]') $(tr 3 't["numbered"]')"
	[ "$E" = "0 0 0" ] && break
	sleep 0.2; k=$((k+1))
done
[ "$E" = "0 0 0" ] && ok "2. the deletes stop: every node's numbered entries go to 0" ||
	bad "2. after the driver: numbered entries $E (want 0 0 0)"

# 3. node 3 frozen past the purge while the deletes go on
R0=$(tr 1 't["replayed"]'); G0=$(tr 3 'c["acks"]["gap_asked"]')
drive 20 26 100000 > "$D/d2" &
DP=$!
sleep 3
kill -STOP $P3
sleep 10
H=$(tr 1 't["numbered"]')
kill -CONT $P3
[ "${H:-0}" -ge 150 ] 2>/dev/null &&
	ok "3. node 3 frozen 10 s (purged at 6, still a member): node 1 kept the $H deletes it has not confirmed" ||
	bad "3. node 3 frozen 10 s: node 1 holds ${H:-?} numbered entries (want 150 or more - 20 a second): $(grep -m2 'went silent\|purged' "$D/n1.log" | tr '\n' ' ')"

# 4. back: it gets them, confirms, the tables shrink
wait $DP; DP=
N2=$(cat "$D/d2")
k=0; while [ $k -lt 75 ]; do
	E="$(tr 1 't["numbered"]') $(tr 3 't["numbered"]')"
	[ "$E" = "0 0" ] && break
	sleep 0.2; k=$((k+1))
done
[ "$E" = "0 0" ] && ok "4. node 3 back: it confirmed what it missed - nodes 1 and 3 forgot everything" ||
	bad "4. node 3 back: numbered entries on nodes 1 and 3: $E (want 0 0); node 3's marks: $(tr 3 'c["acks"]["marks"]')"
# what it missed reached it: asked for past its marks (AT-7), or replayed
# by node 1's catch-up where its marks did not cover it (v0.7.3-rc1: the
# catch-up skipped every numbered delete, and a partition's were lost)
R1=$(tr 1 't["replayed"]'); G1=$(tr 3 'c["acks"]["gap_asked"]')
{ [ "${G1:-0}" -gt "${G0:-0}" ] || [ "${R1:-0}" -gt "${R0:-0}" ]; } 2>/dev/null &&
	ok "4. what node 3 missed reached it: $G0 -> $G1 requests by its marks, $R0 -> $R1 replayed by node 1's catch-up" ||
	bad "4. node 3 missed deletes but neither asked ($G0 -> $G1) nor was replayed to ($R0 -> $R1)"
L=$(python3 - "$N2" <<'PY' 2>/dev/null
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.148.3", 19423), 10))
n = 0
for i in range(100000, int(sys.argv[1])):
    r = pcnative.call(f, "get", col="c", key="g:%d" % i)
    if (r.get("result") or {}).get("found"):
        n += 1
print(n)
PY
)
[ "$L" = 0 ] && ok "4. node 3 holds none of the $((N2 - 100000)) keys deleted around its freeze" ||
	bad "4. node 3 holds ${L:-?} of the $((N2 - 100000)) deleted keys"

# 5. expiries: let go, or kept when the TTL was shortened
F0="$(tr 1 't["forgotten_expiries"]') $(tr 2 't["forgotten_expiries"]') $(tr 3 't["forgotten_expiries"]')"
python3 - <<'PY'
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.148.1", 19421), 10))
for i in range(100):
    pcnative.call(f, "set", col="c", key="e:%d" % i, value="v", ttl=2)
    pcnative.call(f, "set", col="c", key="k:%d" % i, value="v")
for i in range(100):
    pcnative.call(f, "expire", col="c", key="k:%d" % i, ttl=2)
PY
set -- $F0
k=0; while [ $k -lt 150 ]; do
	G="$(tr 1 't["forgotten_expiries"]') $(tr 2 't["forgotten_expiries"]') $(tr 3 't["forgotten_expiries"]')"
	set -- $F0 $G
	[ $(($4 - $1)) -ge 100 ] && [ $(($5 - $2)) -ge 100 ] && [ $(($6 - $3)) -ge 100 ] && break
	sleep 0.2; k=$((k+1))
done
sleep 2
X="$(tr 1 't["expiries"]') $(tr 2 't["expiries"]') $(tr 3 't["expiries"]')"
set -- $F0 $G
[ $(($4 - $1)) -ge 100 ] && [ $(($5 - $2)) -ge 100 ] && [ $(($6 - $3)) -ge 100 ] && [ "$X" = "100 100 100" ] &&
	ok "5. (c): the plain TTLs' expiries let go after the hold on every node; the shortened hundred kept ($X)" ||
	bad "5. (c): expiries let go $(($4 - $1))/$(($5 - $2))/$(($6 - $3)) (want 100 each), held $X (want the 100 shortened on each)"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombforgettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
