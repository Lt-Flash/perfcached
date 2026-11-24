#!/bin/sh
# tombspacetest.sh - a numbering space of a run that is over (a node restarted) is forgotten once it is
# held whole, confirmed and retains nothing: the count stays bounded across restarts instead of
# growing by one a restart for good.  Usage: test/tombspacetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pctsp.XXXXXX)
P1= P2=
trap 'for p in $P1 $P2; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19441 19442; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombspacetest: port $p already bound" >&2; exit 1; }
done
RETAIN=20
for i in 1 2; do
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
tcp = 127.0.149.$i:1944$i
plaintext = loopback
[cluster]
multicast = 239.255.77.168:17288
advertise = 127.0.149.$i
mode = eager
collections = c
tombstone_retain_s = $RETAIN
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
start() { # start <i>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=$!"
	k=0; while [ $k -lt 200 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1; }
}
cs() { # cs <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.149.%d" % i, 19440 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
dels() { # dels <i> <tag>: ten set+delete pairs through node i - numbered in its run
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.149.%d" % i, 19440 + i), 10))
n = 0
for k in range(10):
    key = "%s-%d" % (sys.argv[2], k)
    pcnative.call(f, "set", col="c", key=key, value="v")
    n += 1 if (pcnative.call(f, "del", col="c", key=key).get("result") or {}).get("deleted") else 0
print(n)
PY
}
both() { [ "$(cs 1 'c.get("peers_up")')" = 1 ] && [ "$(cs 2 'c.get("peers_up")')" = 1 ] &&
	[ "$(cs 1 'c["acks"]["fleet"]["confirming"]')" = True ]; }
waitfor() { w=0; while [ $w -lt $(($1 * 4)) ]; do eval "$2" && return 0; sleep 0.25; w=$((w+1)); done; return 1; }

start 1 && start 2 || { echo "tombspacetest: $pass passed, $fail failed"; exit 1; }
waitfor 20 both || { bad "the pair did not form and confirm: $(cs 1 'c.get("acks")')"; exit 1; }
[ "$(dels 1 a)" = 10 ] && [ "$(dels 2 b0)" = 10 ] && ok "deletes numbered on both nodes" || bad "the deletes were not all applied"

# five restarts of node 2, each run numbering its own deletes: a new space on both nodes each time
peak=0
for r in 1 2 3 4 5; do
	kill -TERM $P2; wait $P2 2>/dev/null; P2=
	start 2 || break
	waitfor 20 both || bad "restart $r: the pair did not confirm again"
	dels 2 "b$r" > /dev/null
	waitfor 5 '[ "$(cs 1 "c[\"acks\"][\"applied\"]")" -ge $((20 + r * 10)) ]'
	s=$(cs 1 'c["acks"]["spaces"]'); [ "${s:-0}" -gt $peak ] && peak=$s
done
# the control: inside retain_s every dead run's space is still held, so the count did grow
[ "$peak" -ge 5 ] && ok "control: node 1 held $peak spaces after five restarts of node 2, inside retain_s" \
	|| bad "control: node 1 peaked at $peak spaces - want at least 5 (a run's space each)"

# past retain_s the dead runs' spaces go: node 1 keeps its own and node 2's current, node 2 the same
waitfor $((RETAIN + 30)) '[ "$(cs 1 "c[\"acks\"][\"spaces\"]")" -le 2 ] && [ "$(cs 2 "c[\"acks\"][\"spaces\"]")" -le 2 ]'
s1=$(cs 1 'c["acks"]["spaces"]') s2=$(cs 2 'c["acks"]["spaces"]')
[ "${s1:-9}" -le 2 ] && [ "${s2:-9}" -le 2 ] && ok "the dead runs' spaces are forgotten: node 1 holds $s1, node 2 $s2 (dropped $(cs 1 'c["acks"]["dropped"]') / $(cs 2 'c["acks"]["dropped"]'))" \
	|| bad "spaces still held past retain_s: node 1 $s1, node 2 $s2 - want 2 at most each"
grep -q "numbering space(s) of runs that are over forgotten" "$D/n1.log" && ok "node 1 logs it" || bad "no 'forgotten' line on node 1"

# and they stay gone over one more restart: node 2's WAL replay brings back nothing for good
kill -TERM $P2; wait $P2 2>/dev/null; P2=
start 2 && waitfor 20 both
dels 2 b9 > /dev/null
waitfor $((RETAIN + 30)) '[ "$(cs 1 "c[\"acks\"][\"spaces\"]")" -le 2 ] && [ "$(cs 2 "c[\"acks\"][\"spaces\"]")" -le 2 ]'
s1=$(cs 1 'c["acks"]["spaces"]') s2=$(cs 2 'c["acks"]["spaces"]')
[ "${s1:-9}" -le 2 ] && [ "${s2:-9}" -le 2 ] && ok "one restart more: still bounded (node 1 $s1, node 2 $s2)" \
	|| bad "after one restart more: node 1 $s1, node 2 $s2 spaces"
# the deletes still hold: a key deleted in a forgotten run reads absent on both
g=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
n = 0
for i in (1, 2):
    f = pcnative.wrap(socket.create_connection(("127.0.149.%d" % i, 19440 + i), 10))
    for t in ("a", "b0", "b1", "b5"):
        for k in range(10):
            n += 1 if (pcnative.call(f, "get", col="c", key="%s-%d" % (t, k)).get("result") or {}).get("value") else 0
print(n)
PY
)
[ "$g" = 0 ] && ok "every deleted key still reads absent on both nodes" || bad "$g deleted key(s) read back"
e=$(cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | grep -cv 'an fsync has been stuck')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || { bad "$e ERROR/CRIT lines"; cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | head -5; }
for p in $P1 $P2; do kill $p 2>/dev/null; wait $p 2>/dev/null; done; P1= P2=
echo "tombspacetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
