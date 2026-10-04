#!/bin/sh
# lifeguardtest.sh - S36 step e1: a node that was itself stalled does not
# blame its healthy peers when it wakes (Lifeguard's local health).
#
# A stopped process, a CPU-starved one or a paused VM wakes with its
# peers' beats unread in its socket.  It used to purge every peer as
# silent and elect itself - one stalled node accusing a healthy fleet.
# Now a node whose own tick ran late holds purges and master-dead
# decisions for as long as the stall lasted while fresh beats arrive.
#   three nodes; node 3 SIGSTOPped for 9 s (past the 6 s purge and the
#   8 s master-dead limits), then continued:
#   1  node 3 says its own loop ran late (and /stats self_late moves)
#   2  node 3 purges neither peer as silent and does not make itself
#      master
#   3  the fleet is three again (the others rightly saw node 3 silent)
# Fail-first: the build before e1 has node 3 purge both peers.
# Usage: test/lifeguardtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pclg.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; kill -CONT "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18240

conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 16
[secrets]
client = lg-client-secret
cluster = lg-cluster-secret
[listen]
tcp = 127.0.0.1:$((18250 + $1))
http = 127.0.0.1:$((18260 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.36.1, 127.0.36.2, 127.0.36.3
advertise = 127.0.36.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
st() {
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18260 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
for n in 1 2 3; do conf $n; "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 & echo $! > "$D/n$n.pid"; done
wait_for 40 '[ "$(up 1)$(up 2)$(up 3)" = 222 ]' || { bad "the fleet did not form"; echo "lifeguardtest: $pass passed, $fail failed"; exit 1; }
sleep 2
P3=$(cat "$D/n3.pid")
L0=$(wc -l < "$D/n3.log")
kill -STOP $P3; sleep 9; kill -CONT $P3
sleep 6
tail -n +$((L0 + 1)) "$D/n3.log" > "$D/n3.after"
grep -q "own loop ran" "$D/n3.after" && [ "$(st 3 'r["cluster"]["self_late"]')" -ge 1 ] 2>/dev/null \
	&& ok "node 3 says its own loop ran late (self_late $(st 3 'r["cluster"]["self_late"]'))" \
	|| bad "node 3 did not notice its own stall"
if grep -q "went silent" "$D/n3.after"; then
	bad "node 3 purged a healthy peer: $(grep -m1 'went silent' "$D/n3.after" | cut -c1-120)"
else
	ok "node 3 purged neither peer"
fi
if grep -qE "elected master|claimed mastership" "$D/n3.after"; then
	bad "node 3 made itself master after its own stall"
else
	ok "node 3 did not make itself master"
fi
wait_for 30 '[ "$(up 1)$(up 2)$(up 3)" = 222 ]' && ok "the fleet is three again" \
	|| bad "the fleet did not re-form ($(up 1)/$(up 2)/$(up 3))"
for f in "$D"/*.pid; do kill "$(cat "$f")" 2>/dev/null; done
echo "lifeguardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
