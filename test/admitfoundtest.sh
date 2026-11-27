#!/bin/sh
# admitfoundtest.sh - who founds a fleet (the lowest identity, admitting from its queue), a joiner
# arriving while the fleet has no master (told ELECTING, it never founds), and member-list asks.
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
[ -f "$SHIM" ] || { echo "admitfoundtest: $SHIM is missing (make netcutshim.so)" >&2; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcadf.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.86.13 MP=24933
for p in 24931 24932 $MP; do
	ss -lntu 2>/dev/null | grep -qE "127\.0\.113\.[0-9]+:$p[[:space:]]" && { echo "admitfoundtest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <n>: a blank node
	rm -rf "$D/s$1"; mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = af-client-secret
cluster = af-cluster-secret
[listen]
tcp = 127.0.113.$1:24931
http = 127.0.113.$1:24932
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.113.$1
max_datagram = 200
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
start() {
	: > "$D/cut$1"
	LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"
}
st() {
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.113.%s:24932/stats" % sys.argv[1],timeout=3).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
role() { st "$1" 'r["cluster"]["role"]'; }
up() { st "$1" 'r["cluster"]["peers_up"]'; }
cid() { st "$1" 'r["cluster"]["cluster_id"]'; }
ident() { grep -m1 -o 'cluster: identity [0-9a-f]\{32\} uuid' "$D/n$1.log" | awk '{print $3}'; }
wait_for() { # wait_for <s> <condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
fleet() { # fleet <nodes...>: every one a member or the master, one cluster id, peers_up = N-1
	n=$#; want=$((n - 1)); ids=
	for i in "$@"; do
		[ "$(up $i)" = $want ] || return 1
		case "$(role $i)" in master|member) ;; *) return 1;; esac
		ids="$ids
$(cid $i)"
	done
	[ "$(echo "$ids" | grep -v '^$' | sort -u | wc -l)" = 1 ]
}

echo "--- A: eight blank nodes, lowest address (and identity) first"
for n in 1 2 3 4 5 6 7 8; do conf $n; done
for n in 1 2 3 4 5 6 7 8; do start $n; sleep 0.2; done
if wait_for 60 'fleet 1 2 3 4 5 6 7 8'; then
	ok "A. one fleet of eight, one cluster id"
else
	bad "A. no single fleet: peers $(for n in 1 2 3 4 5 6 7 8; do printf '%s ' "$(up $n)"; done)roles $(for n in 1 2 3 4 5 6 7 8; do printf '%s ' "$(role $n)"; done); node 1: $(tail -2 "$D/n1.log" | tr '\n' '|' | cut -c1-240)"
fi
[ "$(st 1 'r["cluster"]["epoch"]["committed"]')" = 4 ] && ok "A. the fleet speaks wire epoch 4 from its first start" \
	|| bad "A. node 1 speaks epoch $(st 1 'r["cluster"]["epoch"]["committed"]')"
M=0
for n in 1 2 3 4 5 6 7 8; do [ "$(role $n)" = master ] && M=$n; done
LOW=$(for n in 1 2 3 4 5 6 7 8; do echo "$(ident $n) $n"; done | sort | head -1 | awk '{print $2}')
NI=$(for n in 1 2 3 4 5 6 7 8; do ident $n; done | grep -c .)   # all 8, or LOW means nothing
[ "$NI" = 8 ] && [ "$M" != 0 ] && [ "$M" = "$LOW" ] \
	&& ok "A. the master is node $M, the lowest identity ($(ident $M))" \
	|| bad "A. the master is node $M ($(ident $M)), the lowest identity is node $LOW ($(ident $LOW)); $NI of 8 identities read"
F=$(grep -l "founded the cluster" "$D"/n[1-8].log 2>/dev/null | wc -l)
[ "$F" = 1 ] && ok "A. one node founded" || bad "A. $F nodes logged that they founded: $(grep -l "founded the cluster" "$D"/n[1-8].log | tr '\n' ' ')"
NA=$(grep -c "cluster: admitting 127.0.113.[0-9]* from the queue" "$D/n$M.log" 2>/dev/null)
N1=$(grep -c "cluster: admitting 127.0.113.[0-9]* from the queue (1 in flight" "$D/n$M.log" 2>/dev/null)
[ "$NA" -ge 7 ] && [ "$NA" = "$N1" ] \
	&& ok "A. the founder admitted from its queue, one in flight at a time ($NA admissions; $(grep -l 'queued for admission' "$D"/n[1-8].log 2>/dev/null | wc -l) joiner(s) told QUEUED meanwhile)" \
	|| bad "A. admissions from the queue: $NA, of them with 1 in flight: $N1"
NP=$(grep -c "cluster: the member list for node [0-9]* (127.0.113.[0-9]*) goes in 2 parts" "$D/n$M.log" 2>/dev/null)
NG=$(grep -l "cluster: ASSIGN part 2 of 2" "$D"/n[1-8].log 2>/dev/null | wc -l)
[ "$NP" -ge 2 ] && [ "$NG" -ge 2 ] \
	&& ok "A. member lists past one datagram went in parts: $NP split by the founder, $NG joiner(s) took part 2" \
	|| bad "A. split member lists: the founder logged $NP, joiners that took part 2: $NG"

echo "--- B: the master killed; a ninth node arrives while the members have no master"
OTHERS=$(for n in 1 2 3 4 5 6 7 8; do [ $n != "$M" ] && printf '%s ' $n; done)
[ "$M" != 0 ] && kill -9 "$(cat "$D/n$M.pid")" 2>/dev/null; rm -f "$D/n$M.pid"
sleep 4
conf 9; start 9
if wait_for 60 "fleet $OTHERS 9"; then
	ok "B. node 9 joined the seven survivors: one fleet, one cluster id"
else
	bad "B. no fleet of the survivors and node 9: node 9 $(role 9), peers $(up 9), cid $(cid 9) vs $(cid ${OTHERS%% *})"
fi
grep -q "cluster: ASSIGN part 2 of 2" "$D/n9.log" && ok "B. node 9 took its member list in two parts" \
	|| bad "B. node 9 took no second part: $(grep -m1 'admitted as node' "$D/n9.log" | cut -c1-160)"
grep -q "says it is electing" "$D/n9.log" && ok "B. node 9 was told the fleet is electing ($(grep -m1 -o 'node [0-9]* of its [0-9]* member(s), term [0-9]*' "$D/n9.log"))" \
	|| bad "B. node 9 was never told ELECTING"
grep -q "founded the cluster" "$D/n9.log" && bad "B. node 9 founded: $(grep -m1 'founded the cluster' "$D/n9.log")" \
	|| ok "B. node 9 never founded"
NM=0
for n in $OTHERS; do [ "$(role $n)" = master ] && NM=$n; done
[ "$NM" != 0 ] && [ "$(role 9)" = member ] && ok "B. the members elected node $NM; node 9 is its member" \
	|| bad "B. masters after the kill: survivors' master $NM, node 9 $(role 9)"
if [ "$NM" != 0 ]; then
	lo=$(grep -n "cluster: admission open" "$D/n$NM.log" | head -1 | cut -d: -f1)
	la=$(grep -n "cluster: admitting 127.0.113.9 from the queue" "$D/n$NM.log" | head -1 | cut -d: -f1)
	[ -n "$lo" ] && [ -n "$la" ] && [ "$la" -gt "$lo" ] \
		&& ok "B. node $NM admitted node 9 from its queue after its gate opened ($(grep -c 'cluster: admission held' "$D/n$NM.log") held line(s) first)" \
		|| bad "B. node $NM: gate opened at line ${lo:-none}, node 9 admitted at line ${la:-none}"
fi

echo "--- C: a member cut off from node 9 asks the master for the list"
A=0
for n in $OTHERS; do [ "$n" != "$NM" ] && { A=$n; break; }; done
asked() { st "$1" 'r["cluster"]["members_asked"]'; }
sent() { st "$1" 'r["cluster"]["members_sent"]'; }
if [ "$A" != 0 ] && [ "$NM" != 0 ]; then
	a0=$(asked $A); s0=$(sent $NM)
	echo "cut 127.0.113.9 rst" > "$D/cut$A"; echo "cut 127.0.113.$A rst" > "$D/cut9"
	wait_for 30 '[ "$(up $A)" = 6 ] && [ "$(asked $A)" -gt "$a0" ] && [ "$(sent $NM)" -gt "$s0" ]' \
		&& ok "C. node $A, cut off from node 9, asked the master for the list ($a0 -> $(asked $A)) and node $NM answered ($s0 -> $(sent $NM))" \
		|| bad "C. node $A peers $(up $A), asked $a0 -> $(asked $A), node $NM sent $s0 -> $(sent $NM)"
	sleep 6
	[ "$(up $A)" = 6 ] && ok "C. node 9 not revived on the master's word (node $A still counts 6 peers up)" \
		|| bad "C. node $A counts $(up $A) peers up while cut off from node 9"
	: > "$D/cut$A"; : > "$D/cut9"
	wait_for 30 '[ "$(up $A)" = 7 ]' && ok "C. healed: node $A hears node 9 again" || bad "C. healed, node $A counts $(up $A) peers up"
	sleep 3; a1=$(asked $A); sleep 10
	[ "$(asked $A)" = "$a1" ] && ok "C. and asks no more ($a1 in all)" || bad "C. node $A still asking: $a1 -> $(asked $A)"
else
	bad "C. no member to cut (master $NM, member $A)"
fi

for n in 1 2 3 4 5 6 7 8 9; do
	grep -h 'an fsync has been stuck' "$D/n$n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	grep CRIT "$D/n$n.log" 2>/dev/null | grep -qv 'an fsync has been stuck' && bad "node $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v 'an fsync has been stuck' | head -n 1 | cut -c1-160)"
done
echo "admitfoundtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
