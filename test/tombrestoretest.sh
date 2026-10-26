#!/bin/sh
# tombrestoretest.sh - 12ny AT-12 (f): a member restored from an older
# snapshot does not get back the keys deleted since.
#
# The fleet forgets a delete once every member has confirmed it (AT-5).  A
# member then restored from a backup older than those deletes holds the
# keys again, and nobody holds the deletes to replay to it.  Its marks say
# so - the restored `acks` file is as old as the snapshot - and S392's fast
# path refuses it (TOMBFP_BEHIND), so it walks (B4): every record it brought
# back is asked about, and the fleet holds none of the deleted keys.  Three
# eager nodes; node 3 takes snapshots (a clean stop writes one):
#   1. 300 keys; node 3 stopped cleanly and its state (snapshot, acks, WAL)
#      copied aside - the backup; node 3 back
#   2. 200 of the keys deleted through node 1, confirmed by all three and
#      forgotten by node 1
#   3. node 3 stopped and restored from the backup: its fast path was
#      refused because deletes were forgotten past its marks
#   4. it walked: node 3 holds none of the 200 and all 100 kept, and nodes 1
#      and 2 hold none of the 200 either
# FAIL-FIRST: without the marks' check (AT-5's TOMBFP_BEHIND), the witness's
# table held none of the 200 - forgotten - and its fast path answered OK:
# node 3 kept them.
# Usage: test/tombrestoretest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctrest.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19591 19592 19593; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombrestoretest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	if [ $i = 3 ]; then SAVE="save = 900 1"; else SAVE="save = off"; fi
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = tz-client-secret
cluster = tz-cluster-secret
[listen]
tcp = 127.0.155.$i:1959$i
plaintext = loopback
[cluster]
multicast = 239.255.77.176:17296
advertise = 127.0.155.$i
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
start() {
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & PIDS="$PIDS $!"; eval "P$1=$!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
stopn() { eval "kill -TERM \$P$1; wait \$P$1 2>/dev/null"; }
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.155.%d" % i, 19590 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <set|del> <from> <to>, through node 1
	python3 - "$@" <<'PY'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.155.1", 19591), 10))
for i in range(int(sys.argv[2]), int(sys.argv[3])):
    if sys.argv[1] == "set":
        pcnative.call(f, "set", col="c", key="z:%d" % i, value="v")
    else:
        pcnative.call(f, "del", col="c", key="z:%d" % i)
PY
}
held() { # held <node> <from> <to>
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.155.%d" % n, 19590 + n), 10))
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key="z:%d" % i).get("result") or {}).get("found")))
PY
}
gate() {
	k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
	python3 "$T/waitready.py" 127.0.155.1:19591 127.0.155.2:19592 127.0.155.3:19593 || bad "a node is not ready"
}
for i in 1 2 3; do start $i; done
gate

# 1. 300 keys; node 3's backup
ops set 0 300; sleep 2
stopn 3
cp -a "$D/s3" "$D/s3.bak" && cp -a "$D/w3" "$D/w3.bak"
[ -s "$D/w3.bak/acks" ] && [ "$(held 1 0 300)" = 300 ] &&
	ok "1. 300 keys; node 3 stopped cleanly, its snapshot, acks and WAL copied aside" ||
	bad "1. the backup: acks $(ls "$D/w3.bak" | tr '\n' ' '); node 1 holds $(held 1 0 300) of 300"
start 3; gate

# 2. 200 deletes, confirmed by all three, forgotten
ops del 0 200
k=0; while [ $k -lt 100 ] && [ "$(st 1 'c["tombstones_retained"]["numbered"]')" != 0 ]; do sleep 0.2; k=$((k+1)); done
N1=$(st 1 'c["tombstones_retained"]["numbered"]'); F1=$(st 1 'c["tombstones_retained"]["forgotten"]')
[ "$N1" = 0 ] && [ "${F1:-0}" -ge 200 ] 2>/dev/null && [ "$(held 3 0 200)" = 0 ] &&
	ok "2. 200 deletes: node 3 applied them, node 1 forgot all $F1 once all three confirmed" ||
	bad "2. after 200 deletes: node 1 keeps ${N1:-?}, forgot ${F1:-?} (want 0 and 200 or more); node 3 holds $(held 3 0 200) of them"

# 3. node 3 restored from the backup
stopn 3
rm -rf "$D/s3" "$D/w3"; cp -a "$D/s3.bak" "$D/s3"; cp -a "$D/w3.bak" "$D/w3"
start 3
k=0; while [ $k -lt 150 ] && [ -z "$(st 3 'c.get("reconcile_fast_why") or ""')" ] && [ "$(st 3 'c.get("reconcile_open")')" != False ]; do sleep 0.2; k=$((k+1)); done
WHY=$(st 3 'c.get("reconcile_fast_why")')
case "$WHY" in *forgot*) ok "3. node 3 restored from the older backup: its fast path was refused - $WHY" ;;
	*) bad "3. node 3's fast path after the restore: '${WHY:-none}' (want it refused because deletes were forgotten past its marks); fast $(st 3 'c.get("reconcile_fast")'); $(grep -m2 -h 'fast path\|forgot' "$D/n3.log" "$D/n1.log" "$D/n2.log" | tr '\n' ' ' | cut -c1-300)" ;; esac

# 4. it walked: none of the 200 anywhere
k=0; while [ $k -lt 150 ] && [ "$(held 3 0 200)" != 0 ]; do sleep 0.2; k=$((k+1)); done
sleep 2
G3=$(held 3 0 200); L3=$(held 3 200 300); G1=$(held 1 0 200); G2=$(held 2 0 200)
[ "$G3" = 0 ] && [ "$L3" = 100 ] && [ "$G1 $G2" = "0 0" ] &&
	ok "4. node 3 walked: it holds none of the 200 deleted keys and all 100 kept; nodes 1 and 2 hold none of the 200" ||
	bad "4. after the restore: node 3 holds $G3 of the 200 deleted (want 0) and $L3 of the 100 kept (want 100); nodes 1/2 hold $G1/$G2 of the 200 (want 0 0)"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombrestoretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
