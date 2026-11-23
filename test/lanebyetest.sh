#!/bin/sh
# lanebyetest.sh - a master's GOODBYE, its return, and a member's restart inside the purge window are
# no UDP block: no node forces its TCP lane (lingershim holds the stop 4 s so the lane still answers).
set -u
BIN=$(readlink -f "${1:-./perfcached}")
SHIM=$(readlink -f "${2:-./lingershim.so}")
# under ASan the sanitizer runtime must come first in the preload list, or
# the daemon refuses to start (eagerparttest's spelling); and it forms slower
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
FW=40; [ -n "${SANRT:-}" ] && FW=120
[ -f "$SHIM" ] || { echo "lanebyetest: no $SHIM - make lingershim.so"; exit 1; }
D=$(mktemp -d /var/tmp/pclb.XXXXXX)
cleanup() { for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"; }
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
CPORT=19730
for n in 1 2 3; do
	ss -ltnu 2>/dev/null | grep -qE "127\.0\.74\.$n:1973[0-3][[:space:]]" && { echo "lanebyetest: port busy on 127.0.74.$n" >&2; exit 1; }
done

conf() { # conf <n>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = lb-client-secret
cluster = lb-cluster-secret
[listen]
tcp = 127.0.74.$1:19731
http = 127.0.74.$1:19733
plaintext = loopback
[cluster]
discovery = unicast
port = $CPORT
seeds = 127.0.74.1, 127.0.74.2, 127.0.74.3
advertise = 127.0.74.$1
expect = 3
mode = eager
collections = 0
[wal]
dir = $D/w$1
probe = no
fsync = everysec
[collection 0]
buckets_log2 = 10
CONF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1" "$D/w$1"
}
start() {
	LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_LINGER_MARK="$D/linger" PC_LINGER_LOG="$D/linger.log" PC_LINGER_S=4 \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() { # st <n> <python expr over r> -> value or ?
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.74.%s:19733/stats" % sys.argv[1],timeout=3).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up()     { st "$1" 'r["cluster"]["peers_up"]'; }
role()   { st "$1" 'r["cluster"]["role"]'; }
forced() { st "$1" 'r["cluster"].get("lane",{}).get("forced",0)'; }
wait_for() { k=0; while [ $k -lt $(($1 * 2)) ]; do eval "$2" && return 0; sleep 0.5; k=$((k+1)); done; return 1; }

for n in 1 2 3; do conf $n; start $n; done
if ! wait_for $FW '[ "$(up 1)$(up 2)$(up 3)" = 222 ]'; then
	bad "the fleet did not form ($(up 1)/$(up 2)/$(up 3))"; echo "lanebyetest: $pass passed, $fail failed"; exit 1
fi
M=0; for n in 1 2 3; do [ "$(role $n)" = master ] && M=$n; done
[ "$M" != 0 ] || { bad "no master among the three"; echo "lanebyetest: $pass passed, $fail failed"; exit 1; }
O1=$(( M % 3 + 1 )); O2=$(( O1 % 3 + 1 ))
for n in 1 2 3; do eval "L$n=\$(wc -l < \"$D/n$n.log\")"; done

echo "--- 1: the master (node $M) stops cleanly"
touch "$D/linger"
kill -TERM "$(cat "$D/n$M.pid")"; wait "$(cat "$D/n$M.pid")" 2>/dev/null; rm -f "$D/n$M.pid"
rm -f "$D/linger"
grep -q "held .*dump.rdb" "$D/linger.log" 2>/dev/null \
	&& ok "the departing master lingered after its goodbye (its shutdown snapshot held 4 s - the PROD window)" \
	|| bad "the shim never held the shutdown snapshot - the window was not reproduced, so this run proves nothing"
wait_for 20 '[ "$(role $O1)" = master ] || [ "$(role $O2)" = master ]' \
	&& ok "the two left elected a new master" || bad "no new master: node $O1 $(role $O1), node $O2 $(role $O2)"
sleep 4                                     # past MASTER_DEAD_MS and the lane probe
for n in $O1 $O2; do eval "tail -n +\$((L$n + 1)) \"$D/n$n.log\"" > "$D/n$n.after"; done
if grep -h "has sent no UDP" "$D/n$O1.after" "$D/n$O2.after" > "$D/err" 2>/dev/null; then
	bad "a member took the master's goodbye for a UDP block: $(head -1 "$D/err" | sed 's/.*ERROR: //' | cut -c1-120)"
else
	ok "neither member took the master's goodbye for a UDP block"
fi
[ "$(forced $O1)$(forced $O2)" = 00 ] && ok "and neither forced its lane" \
	|| bad "forced lanes after the goodbye: node $O1 $(forced $O1), node $O2 $(forced $O2)"

echo "--- 2: it comes back"
# the lines since the restart: a cold start with two founders can log one at formation, not the return
for n in 1 2 3; do eval "K$n=\$(wc -l < \"$D/n$n.log\")"; done
start $M
wait_for $FW '[ "$(up 1)$(up 2)$(up 3)" = 222 ]' && ok "three again" || bad "the fleet did not re-form ($(up 1)/$(up 2)/$(up 3))"
sleep 3
for n in 1 2 3; do eval "tail -n +\$((K$n + 1)) \"$D/n$n.log\"" > "$D/n$n.back"; done
[ "$(forced 1)$(forced 2)$(forced 3)" = 000 ] && ! grep -q "has sent no UDP" "$D"/n*.back \
	&& ok "no node has a forced lane, and none logged a UDP block since it came back" \
	|| bad "forced lanes $(forced 1)/$(forced 2)/$(forced 3); UDP-block lines since the restart: $(grep -c "has sent no UDP" "$D"/n*.back | tr '\n' ' ')"

echo "--- 3: a member killed and started again inside the purge window"
# its new process answers the lane's TCP probe on the old address: that is no UDP block
X=0; for n in 1 2 3; do [ "$(role $n)" = member ] && X=$n; done
if [ "$X" = 0 ]; then
	bad "no member to kill: roles $(role 1)/$(role 2)/$(role 3)"
else
	for n in 1 2 3; do eval "J$n=\$(wc -l < \"\$D/n\$n.log\")"; done
	kill -9 "$(cat "$D/n$X.pid")"; wait "$(cat "$D/n$X.pid")" 2>/dev/null; rm -f "$D/n$X.pid"
	sleep 2
	start $X
	wait_for $FW '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(st $X "r[\"state\"]")" = ready ]' \
		&& ok "node $X is back and ready, three again" || bad "node $X did not come back: $(up 1)/$(up 2)/$(up 3), $(st $X 'r["state"]')"
	sleep 3
	for n in 1 2 3; do eval "tail -n +\$((J$n + 1)) \"\$D/n\$n.log\"" > "$D/n$n.kill"; done
	if grep -h "has sent no UDP" "$D"/n*.kill > "$D/err3" 2>/dev/null; then
		bad "a peer took node $X's restart for a UDP block: $(head -1 "$D/err3" | sed 's/.*ERROR: //' | cut -c1-120)"
	else
		ok "no peer took node $X's restart for a UDP block"
	fi
	[ "$(forced 1)$(forced 2)$(forced 3)" = 000 ] && ok "and no lane is forced" \
		|| bad "forced lanes after node $X's restart: $(forced 1)/$(forced 2)/$(forced 3)"
fi

echo "lanebyetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
