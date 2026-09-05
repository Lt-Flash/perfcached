#!/bin/sh
# waluniformtest.sh - S73b: a fleet is uniform.  Every node logs a WAL or
# none does; a node that disagrees is refused at the join with a message
# that names the WAL posture, and never becomes a member.  Two WAL nodes
# join each other, a third WAL node joins them, a node without [wal] does
# not.
# usage: test/waluniformtest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcwu.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
for pf in 17991 17992 17993 17994; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "waluniformtest: port $pf already bound" >&2; exit 1; }
done
mk() { # mk <n> <wal: yes|no>
	mkdir -p "$D/w$1"
	{
	cat <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = wu-client-secret
cluster = wu-cluster-secret
[listen]
tcp = 127.0.1.4$1:1799$1
plaintext = loopback
[cluster]
multicast = 239.255.77.173:17273
advertise = 127.0.1.4$1
mode = store
collections = c
[collection c]
buckets_log2 = 12
EOF
	[ "$2" = yes ] && printf '[wal]\ndir = %s\nsegment_mb = 8\nprobe = no\n' "$D/w$1"
	} > "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
members() { # members <n> -> count
	"$CLI" -h 127.0.1.4$1 -p 1799$1 -q -j '{"method":"members"}' 2>/dev/null \
		| python3 -c 'import json,sys
try: print(len(json.load(sys.stdin)["members"]))
except Exception: print(-1)'
}
mk 1 yes; mk 2 yes; mk 3 no; mk 4 yes
start 1 || { echo "node 1 did not start: $(tail -2 "$D/n1.log")"; exit 1; }
start 2 || { echo "node 2 did not start: $(tail -2 "$D/n2.log")"; exit 1; }
sleep 3.5
M=$(members 1)
[ "$M" = 2 ] && ok "two WAL nodes form a fleet" || bad "two WAL nodes: node 1 sees $M member(s)"
start 3 || { echo "node 3 did not start: $(tail -2 "$D/n3.log")"; exit 1; }
sleep 4.5
M=$(members 1)
[ "$M" = 2 ] && ok "a node without [wal] is not admitted (still $M members)" \
	|| bad "S73b: a node without [wal] joined a logging fleet ($M members)"
grep -h "REFUSING peer 127.0.1.43" "$D/n1.log" "$D/n2.log" 2>/dev/null | grep -q "WAL posture differs: it runs WITHOUT a WAL" \
	&& ok "the refusal names the WAL posture" \
	|| bad "S73b: no refusal naming the posture: $(grep -h "REFUSING" "$D/n1.log" "$D/n2.log" 2>/dev/null | head -1 | cut -c1-120)"
M3=$(members 3)
[ "$M3" = 1 ] && ok "the refused node stays alone" || bad "S73b: the refused node sees $M3 member(s)"
start 4 || { echo "node 4 did not start: $(tail -2 "$D/n4.log")"; exit 1; }
sleep 4.5
M=$(members 1)
[ "$M" = 3 ] && ok "a third WAL node joins the logging fleet" || bad "a third WAL node: node 1 sees $M member(s)"
echo "waluniformtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
