#!/bin/sh
# admitconnecttest.sh - an admitted node passes through CONNECTING (every pair proved both ways on the cluster
# socket and the relay port) before ONLINE; one that cannot prove a pair is held there, logged and alerted.
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
CLI=${3:-./perfcli}
[ -f "$SHIM" ] || { echo "admitconnecttest: $SHIM is missing (make netcutshim.so)" >&2; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcadc.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.86.14 MP=24943 SEC=ac-client-secret
for p in 24941 24942 $MP; do
	ss -lntu 2>/dev/null | grep -qE "127\.0\.114\.[0-9]+:$p[[:space:]]" && { echo "admitconnecttest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <n>: a blank node
	rm -rf "$D/s$1"; mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = $SEC
cluster = ac-cluster-secret
[listen]
tcp = 127.0.114.$1:24941
http = 127.0.114.$1:24942
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.114.$1
mode = eager
collections = c
[collection c]
buckets_log2 = 10
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
start() { # start <n> [cut line]: under netcutshim, cut as given from its first datagram
	echo "${2:-}" > "$D/cut$1"
	LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"
}
st() {
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.114.%s:24942/stats" % sys.argv[1],timeout=3).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
role() { st "$1" 'r["cluster"]["role"]'; }
state() { st "$1" 'r["state"]'; }
up() { st "$1" 'r["cluster"]["peers_up"]'; }
stuck() { st "$1" 'r["cluster"]["admission_stuck"]'; }
alerts() { st "$1" '" ".join(a["key"] for a in r["alerts"]["active"])'; }
nid() { st "$1" 'r["cluster"]["node"]'; }
wait_for() { # wait_for <s> <condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
master_of() { for n in "$@"; do [ "$(role $n)" = master ] && { echo $n; return; }; done; echo 0; }

echo "--- 1: three blank nodes - the admitted two go CONNECTING, then ONLINE"
for n in 1 2 3; do conf $n; done
for n in 1 2 3; do start $n; sleep 0.2; done
wait_for 60 '[ "$(up 1)$(up 2)$(up 3)" = 222 ] && [ "$(state 1)$(state 2)$(state 3)" = readyreadyready ]' \
	&& ok "1. one fleet of three, every node READY" \
	|| bad "1. peers $(up 1)/$(up 2)/$(up 3), states $(state 1)/$(state 2)/$(state 3); node 1: $(tail -2 "$D/n1.log" | tr '\n' '|' | cut -c1-240)"
M=$(master_of 1 2 3)
for n in 1 2 3; do
	[ "$n" = "$M" ] && continue
	grep -q "CONNECTING: proving every link before it serves" "$D/n$n.log" && grep -q "cluster: ONLINE - every link" "$D/n$n.log" \
		&& grep -q "node state .* -> connecting" "$D/n$n.log" && grep -qE "node state connecting -> (recovering|ready)" "$D/n$n.log" \
		&& ok "1. node $n: admitted CONNECTING, then ONLINE and READY ($(grep -m1 -o 'answered both ways ([0-9]* ms' "$D/n$n.log"))" \
		|| bad "1. node $n: $(grep -E 'CONNECTING|ONLINE|node state' "$D/n$n.log" | tr '\n' '|' | cut -c1-300)"
done
[ "$(grep -c "cluster: node [0-9]* (127.0.114.[0-9]*) is ONLINE - all 2 pair(s) with it held both ways" "$D/n$M.log")" -ge 1 ] \
	&& ok "1. the master moved them ONLINE once every pair held: $(grep -c 'is ONLINE - all' "$D/n$M.log") of them" \
	|| bad "1. the master's ONLINE lines: $(grep 'is ONLINE' "$D/n$M.log" | tr '\n' '|' | cut -c1-300)"

X=$(for n in 1 2 3; do [ "$n" != "$M" ] && { echo $n; break; }; done)   # below the other member: no standby
echo "--- 2: node 5, cut off from node $X, is held CONNECTING"
conf 5
echo "cut 127.0.114.5 rst" > "$D/cut$X"
start 5 "cut 127.0.114.$X rst"
wait_for 30 '[ "$(state 5)" = connecting ] && [ "$(stuck $M)" = 1 ]' \
	&& ok "2. node 5 is CONNECTING and the master counts it held (admission_stuck 1)" \
	|| bad "2. node 5 state $(state 5), master admission_stuck $(stuck $M)"
NX=$(nid $X)
grep -q "admission stuck - node [0-9]* (127.0.114.5) has been CONNECTING .*node $NX: " "$D/n$M.log" \
	&& ok "2. the master names the pair: $(grep -m1 -o "node $NX: [^;.]*" "$D/n$M.log")" \
	|| bad "2. no stuck line on node 5 naming node $NX: $(grep 'admission stuck' "$D/n$M.log" | tr '\n' '|' | cut -c1-300)"
wait_for 40 'alerts $M | grep -q cluster.admission_stuck' && ok "2. cluster.admission_stuck is raised on the master" \
	|| bad "2. the master's active alerts: $(alerts $M)"
R=$(printf '%s\n' '{"method":"set","params":{"col":"c","key":"held","value":"x"}}' | "$CLI" -h 127.0.114.5 -p 24941 -q 2>&1)   # plaintext = loopback
echo "$R" | grep -q "not READY" && ok "2. node 5 refuses client data while CONNECTING ($R)" || bad "2. node 5 answered a set: $R"

echo "--- 3: node 4 meanwhile - the queue moves on"
conf 4; start 4
wait_for 40 '[ "$(state 4)" = ready ]' && [ "$(state 5)" = connecting ] \
	&& ok "3. node 4 went ONLINE and READY while node 5 is held" \
	|| bad "3. node 4 $(state 4), node 5 $(state 5)"

echo "--- 4: the master killed - node 5 is no candidate"
OTHERS=$(for n in 1 2 3 4; do [ $n != "$M" ] && printf '%s ' $n; done)
kill -9 "$(cat "$D/n$M.pid")" 2>/dev/null; rm -f "$D/n$M.pid"
wait_for 40 '[ "$(master_of $OTHERS)" != 0 ]' && NM=$(master_of $OTHERS) || NM=0
[ "$NM" != 0 ] && [ "$(role 5)" = member ] && ! grep -qE "elected master|founded the cluster" "$D/n5.log" \
	&& ok "4. the members elected node $NM; node 5 (the highest address, CONNECTING) stood for nothing" \
	|| bad "4. new master $NM, node 5 role $(role 5): $(grep -E 'elected master|founded' "$D/n5.log" | head -1)"
wait_for 30 '[ "$(stuck $NM)" = 1 ]' && [ "$(state 5)" = connecting ] \
	&& ok "4. node $NM checked node 5 again and holds it" \
	|| bad "4. node $NM admission_stuck $(stuck $NM), node 5 $(state 5)"

echo "--- 5: the cut healed"
: > "$D/cut$X"; : > "$D/cut5"
wait_for 40 '[ "$(state 5)" = ready ] && [ "$(stuck $NM)" = 0 ]' \
	&& ok "5. node 5 ONLINE and READY; the count is back to 0" \
	|| bad "5. node 5 $(state 5), node $NM admission_stuck $(stuck $NM)"
wait_for 60 '! alerts $NM | grep -q cluster.admission_stuck' && ok "5. and the alert cleared" \
	|| bad "5. alerts still active on node $NM: $(alerts $NM)"

echo "--- 6: node 6, its TCP to node $X cut and its UDP not - held for a TCP path (3c)"
conf 6
start 6 "cut 127.0.114.$X tcp"
wait_for 30 '[ "$(state 6)" = connecting ] && [ "$(stuck $NM)" = 1 ]' \
	&& ok "6. node 6 is CONNECTING and node $NM counts it held" \
	|| bad "6. node 6 state $(state 6), node $NM admission_stuck $(stuck $NM)"
# clrpt is checked before the bulk listener, and max_datagram's default puts it in use
W6=$(grep "admission stuck - node [0-9]* (127.0.114.6) " "$D/n$NM.log" | tail -1 | sed 's/.*not proved both ways: //')
echo "$W6" | grep -qE "node $NX: (clrpt|the bulk listener) did not answer" && ok "6. named: $(echo "$W6" | cut -c1-120)" \
	|| bad "6. node $NM on node 6: ${W6:-no stuck line} (want node $NX's clrpt or bulk listener)"
grep -qE "conn [1-9]" "$D/cutlog6" && ok "6. the cut was delivered ($(cat "$D/cutlog6"))" \
	|| bad "6. the shim refused no connect: $(cat "$D/cutlog6" 2>/dev/null)"
: > "$D/cut6"
wait_for 40 '[ "$(state 6)" = ready ] && [ "$(stuck $NM)" = 0 ]' \
	&& ok "6. its TCP back: ONLINE and READY, the count 0" \
	|| bad "6. node 6 $(state 6), node $NM admission_stuck $(stuck $NM)"

for n in 1 2 3 4 5 6; do
	grep -h 'an fsync has been stuck' "$D/n$n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	grep CRIT "$D/n$n.log" 2>/dev/null | grep -qv 'an fsync has been stuck' && bad "node $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v 'an fsync has been stuck' | head -n 1 | cut -c1-160)"
done
echo "admitconnecttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
