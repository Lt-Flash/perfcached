#!/bin/sh
# refusedpeertest.sh - S241 + S242: a node the fleet REFUSES cannot move
# the fleet's mastership, and the refusal says what differs.
#
# Seen on 245-247 (2026-09-26): a node restarted without its [wal] was
# refused by every member (the WAL posture is in the interchange digest),
# founded a cluster of its own - and its MAPS still reached the fleet,
# whose master stepped down to the higher term (rule 3), re-promoted one
# higher, and so on: 111 -> 180 in five minutes, a member flapping
# between member and backup on the page.  The map plane had no admission
# check; the keepalive doors did.
#
# Two eager nodes with a WAL form a fleet; a third, identical but with no
# WAL, starts beside them.  Over 30 s:
#   1. the fleet keeps ONE master and ONE map term - no step-down in its
#      logs, whatever the refused node claims;
#   2. the refused node stays alone (0 peers);
#   3. the fleet saw its maps and ignored them (map.unadmitted > 0 - the
#      path fired; a quiet 30 s with no maps sent would prove nothing);
#   4. S242: the refusal names our WAL posture (wal=on) and says mode and
#      eager MATCH, so the difference is elsewhere - the old line showed
#      only our placement, which is not what differed.
# FAIL-FIRST: the build before S241 steps the fleet's master down to the
# refused node's maps within the window.
# Usage: test/refusedpeertest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrp.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.77.97
MP=17197
for p in 17371 17372 17373; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "refusedpeertest: port $p already bound" >&2; exit 1; }
done
conf() { # conf <n> <wal: 1|0>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = rp-client-secret
cluster = rp-cluster-secret
[listen]
tcp = 127.0.0.1:1737$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.15.$1
mode = eager
collections = c
[collection c]
buckets_log2 = 12
EOF
	[ "$2" = 1 ] && cat >> "$D/n$1.conf" <<EOF
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() {
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
st() { # st <n>: "role node mapterm peers unadmitted"
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
    s.sendall(sys.stdin.read().encode())
    c = json.loads(s.makefile("rb").readline())["result"]["cluster"]; m = c["map"]
    print(c.get("role"), c.get("master_id", c.get("master", "?")), m.get("term"), c.get("peers_up"), m.get("unadmitted", "-"))
except Exception: print("? ? ? ? ?")' "1737$1" 2>/dev/null || echo "? ? ? ? ?"; }
w() { echo "$1" | cut -d' ' -f"$2"; }

conf 1 1; conf 2 1; conf 3 0
start 1 && sleep 1 && start 2 || { echo "refusedpeertest: the fleet did not start"; exit 1; }
i=0; while [ $i -lt 40 ]; do [ "$(w "$(st 1)" 4)" = 1 ] && [ "$(w "$(st 2)" 4)" = 1 ] && break; sleep 0.5; i=$((i+1)); done
sleep 2
A=$(st 1); B=$(st 2)
M0=""; [ "$(w "$A" 1)" = master ] && M0=1; [ "$(w "$B" 1)" = master ] && M0=2
T0=$(w "$A" 3)
[ -n "$M0" ] && [ "$T0" = "$(w "$B" 3)" ] || { echo "refusedpeertest: the fleet did not form ('$A' | '$B')"; exit 1; }
echo "  fleet formed: master node $M0, map term $T0"

start 3 || { echo "refusedpeertest: node 3 did not start"; exit 1; }
changes=0 maxterm=$T0 s3peers=0
i=0
while [ $i -lt 15 ]; do
	sleep 2
	A=$(st 1); B=$(st 2); C3=$(st 3)
	m=""; [ "$(w "$A" 1)" = master ] && m=1; [ "$(w "$B" 1)" = master ] && m="${m}2"
	[ "$m" = "$M0" ] || changes=$((changes + 1))
	for t in "$(w "$A" 3)" "$(w "$B" 3)"; do [ "$t" -gt "$maxterm" ] 2>/dev/null && maxterm=$t; done
	[ "$(w "$C3" 4)" = 0 ] || s3peers=$((s3peers + 1))
	i=$((i + 1))
done
SD=$(cat "$D/n1.log" "$D/n2.log" | grep -c "stepping down")
[ $changes = 0 ] && [ "$maxterm" = "$T0" ] && [ "$SD" = 0 ] \
	&& ok "30 s beside a refused node: the fleet kept master node $M0 and map term $T0, and never stepped down" \
	|| bad "the refused node moved the fleet: $changes sample(s) with another master, term $T0 -> $maxterm, $SD step-down line(s)"
[ $s3peers = 0 ] && ok "the refused node stayed alone (0 peers in every sample): $(st 3)" \
	|| bad "the refused node was seen with peers in $s3peers sample(s)"
UA=$(w "$(st 1)" 5); UB=$(w "$(st 2)" 5)
[ "$UA" != "-" ] && [ "$UB" != "-" ] && [ $((UA + UB)) -gt 0 ] 2>/dev/null \
	&& ok "and the fleet did receive its maps - and ignored them (map.unadmitted $UA + $UB)" \
	|| bad "map.unadmitted $UA / $UB - either the counter is missing or no map arrived, and then this run proves nothing"
L=$(grep -h "our full interchange config" "$D/n1.log" "$D/n2.log" | head -1)
case "$L" in *"wal=on"*"MATCH"*)
	ok "S242: the refusal names our WAL posture and says mode and eager match: $(echo "$L" | sed 's/.*REFUSING peer //' | cut -c1-110)...";;
	*) bad "S242: the refusal line does not name the WAL posture or the match: '$(grep -h "REFUSING" "$D/n1.log" "$D/n2.log" | head -1 | cut -c1-160)'";; esac
echo "refusedpeertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
