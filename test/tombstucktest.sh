#!/bin/sh
# tombstucktest.sh - 12ny AT-11: a member whose watermark stands still while
# the others confirm is SEEN - in /members, in /stats, in an alert.
#
# Before: deletes kept for a member that confirmed nothing were visible only
# by their symptom, once the cap bit (cluster.tombstones).  Now /members
# lists each member's mark of each origin, how far behind the head and how
# long still; /stats cluster.acks.held_by says which member each space's
# deletes wait for and which one is stuck; cluster.acks_stuck raises.
# Three eager nodes; node 3 announces its first marks for ever
# (PERFCACHED_TEST_FREEZE_MARKS) while it applies as usual; a mark still
# for 3 s counts (PERFCACHED_TEST_ACKS_STUCK_MS):
#   1. deletes through node 1 for 15 s: node 1's /members has node 2 near
#      the head and node 3 far behind it, still
#   2. node 1's held_by names node 3, waiting and stuck, and node 1 keeps
#      every delete since (tombstones_retained.numbered)
#   3. node 1 raises cluster.acks_stuck, naming node 3
#   4. node 3 restarted without the flag: nothing is stuck any more, and
#      node 1 forgets what it kept
# FAIL-FIRST: before AT-11 /members has no marks, /stats no held_by, and no
# alert is raised.
# Usage: test/tombstucktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctstuck.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19501 19502 19503 19511 19512 19513; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombstucktest: port $p already bound" >&2; exit 1; }
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
client = ts-client-secret
cluster = ts-cluster-secret
[listen]
tcp = 127.0.153.$i:1950$i
http = 127.0.153.$i:1951$i
plaintext = loopback
[cluster]
multicast = 239.255.77.172:17292
advertise = 127.0.153.$i
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
start() { # start <i> [env...]
	i=$1; shift
	env PERFCACHED_TEST_ACKS_STUCK_MS=3000 "$@" "$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 & eval "P$i=$!"; PIDS="$PIDS $!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log" | tr '\n' ' ')"; exit 1; }
}
st() { # st <i> <python expr over c = /stats cluster, a = /stats alerts>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.153.%d" % i, 19500 + i), 10))
s = pcnative.call(f, "stats")["result"]; c = s["cluster"]; a = s.get("alerts") or {}
print(eval(sys.argv[2]))
PY
}
mem() { # mem <i> <python expr over m = /members' list>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys, urllib.request
i = int(sys.argv[1])
m = json.loads(urllib.request.urlopen("http://127.0.153.%d:%d/members" % (i, 19510 + i), timeout=5).read())["members"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <set|del> <from> <to>, through node 1
	python3 - "$@" <<'PY'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.153.1", 19501), 10))
for i in range(int(sys.argv[2]), int(sys.argv[3])):
    if sys.argv[1] == "set":
        pcnative.call(f, "set", col="c", key="s:%d" % i, value="v")
    else:
        pcnative.call(f, "del", col="c", key="s:%d" % i)
PY
}
start 1; start 2; start 3 PERFCACHED_TEST_FREEZE_MARKS=1
k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
python3 "$T/waitready.py" 127.0.153.1:19501 127.0.153.2:19502 127.0.153.3:19503 || bad "a node is not ready"
N2=$(mem 1 '[x["node"] for x in m if x["addr"] == "127.0.153.2"][0]')
N3=$(mem 1 '[x["node"] for x in m if x["addr"] == "127.0.153.3"][0]')
ops set 0 600
# the first deletes give node 3 the space - the mark it freezes at
ops del 0 5; sleep 2

# 1. deletes for 15 s; node 3 announces the mark it froze at
j=5; while [ $j -lt 305 ]; do ops del $j $((j + 20)); j=$((j + 20)); sleep 1; done
sleep 2
O1=$(st 1 'c["acks"]["held_by"][0]["origin"] if c["acks"]["held_by"] else ""')
B2=$(mem 1 "[r['behind'] for x in m if x['node'] == $N2 for r in x['acks'] if r['origin'] == '$O1'][0]")
B3=$(mem 1 "[r['behind'] for x in m if x['node'] == $N3 for r in x['acks'] if r['origin'] == '$O1'][0]")
S3=$(mem 1 "[r['still_s'] for x in m if x['node'] == $N3 for r in x['acks'] if r['origin'] == '$O1'][0]")
[ -n "$O1" ] && [ "${B2:-x}" -le 20 ] 2>/dev/null && [ "${B3:-0}" -ge 250 ] 2>/dev/null && [ "${S3:-0}" -ge 10 ] 2>/dev/null &&
	ok "1. /members on node 1: node 2 is $B2 behind the head of origin $O1, node 3 $B3 behind and still for $S3 s" ||
	bad "1. /members on node 1 for origin '${O1:-?}': node 2 behind ${B2:-?} (want 20 or less), node 3 behind ${B3:-?} (want 250 or more), still ${S3:-?} s (want 10 or more); rows: $(mem 1 '[(x["node"], x.get("acks")) for x in m]' | cut -c1-400)"

# 2. held_by names node 3, waiting and stuck; node 1 keeps them
H=$(st 1 "[(h['node'], h['waiting'] >= 250, (h['stuck'] or {}).get('node')) for h in c['acks']['held_by'] if h['origin'] == '$O1']")
R=$(st 1 'c["tombstones_retained"]["numbered"]')
[ "$H" = "[($N3, True, $N3)]" ] && [ "${R:-0}" -ge 250 ] 2>/dev/null &&
	ok "2. node 1's held_by: origin $O1 waits for node $N3, which is stuck; node 1 keeps $R deletes for it" ||
	bad "2. node 1's held_by for $O1: $H (want [($N3, True, $N3)]); numbered kept ${R:-?} (want 250 or more); held_by: $(st 1 'c["acks"]["held_by"]' | cut -c1-300)"

# 3. the alert
k=0; while [ $k -lt 100 ] && [ "$(st 1 'sum(1 for x in a.get("active", []) if x["key"] == "cluster.acks_stuck")')" != 1 ]; do sleep 0.2; k=$((k+1)); done
A=$(st 1 '[x["text"] for x in a.get("active", []) if x["key"] == "cluster.acks_stuck"][:1]')
case "$A" in *"node $N3 "*) ok "3. node 1 raises cluster.acks_stuck: $A" ;;
	*) bad "3. cluster.acks_stuck on node 1: '${A:-none}' (want it naming node $N3); active: $(st 1 '[x["key"] for x in a.get("active", [])]')" ;; esac

# 4. node 3 without the flag: nothing stuck, and node 1 forgets
eval "kill \$P3"; k=0; eval "while kill -0 \$P3 2>/dev/null && [ \$k -lt 100 ]; do sleep 0.1; k=\$((k+1)); done"
start 3
k=0; while [ $k -lt 150 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["tombstones_retained"]["numbered"]')" != 0 ]; }; do sleep 0.2; k=$((k+1)); done
SK=$(st 1 "[h for h in c['acks']['held_by'] if h['stuck']]")
R=$(st 1 'c["tombstones_retained"]["numbered"]')
[ "$SK" = "[]" ] && [ "$R" = 0 ] &&
	ok "4. node 3 restarted without the flag: nothing is stuck, and node 1 forgot every delete it kept" ||
	bad "4. after node 3's restart: stuck entries $SK (want none), node 1 keeps ${R:-?} (want 0)"

for i in 1 2 3; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombstucktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
