#!/bin/sh
# clmapwiretest.sh - the cluster map on a live fleet: the master publishes, everyone adopts at one
# epoch, nothing is refused, the term survives a restart, re-publication advances the epoch.
set -u

BIN=${1:-./perfcached}
SEC=cm-client-secret
D=$(mktemp -d /var/tmp/pccm.XXXXXX)
# daemons die by the pids this script started, each checked against its command line so a reused
# pid never hits a stranger; the name-bound pattern stays as a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17961 17962 17963; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "clmapwiretest: port $pf already bound" >&2; exit 1; }
done

mk() {
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = cm-cluster-secret
[listen]
tcp = 127.0.55.$1:1796$1
[wal]
dir = $D/s$1
segment_mb = 8
probe = no
save = off
[cluster]
multicast = 239.255.77.191:17291
advertise = 127.0.55.$1
mode = store
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
cli() { n=$1; shift; ./perfcli -h 127.0.55.$n -p 1796$n -a $SEC "$@" 2>/dev/null; }
# $2 = dotted path under cluster, e.g. map.term
q() {
	cli $1 -j '{"method":"stats"}' | python3 -c '
import json,sys
p = sys.argv[1].split(".")
try:
    o = json.load(sys.stdin)["cluster"]
    for k in p: o = o[k]
    print(o)
except Exception: print("ERR")' "$2"
}
# role and map.received from ONE stats call: two would double the load on a fleet being timed
rr() {                                 # rr <n> -> "<role> <received>"
	# bounded: an unresponsive node must cost ~3s, not the client
	# library's full patience, or the await above cannot keep its deadline
	timeout 3 ./perfcli -h 127.0.55.$1 -p 1796$1 -a $SEC \
		-j '{"method":"stats"}' 2>/dev/null | python3 -c '
import json,sys
try:
    c = json.load(sys.stdin)["cluster"]
    print(c.get("role", "?"), c.get("map", {}).get("received", -1))
except Exception: print("ERR -1")'
}

await_map() {
	i=0
	while [ $i -lt 80 ]; do
		[ "$(q $1 map.valid)" = "True" ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}

# poll a condition until it holds, up to <secs>: a fixed sleep then ONE check is a coin flip
# against a republication tick
await() {                              # await <secs> <fn>
	# deadline by WALL CLOCK, not iterations: a predicate can block for a whole client timeout
	aw_end=$(( $(date +%s) + $1 )); shift
	# ALWAYS check once, whatever the budget: under set -u an unset reporting variable aborts the file
	while :; do
		"$@" && return 0
		[ "$(date +%s)" -ge "$aw_end" ] && return 1
		sleep 0.25
	done
}

# sample all three epochs TOGETHER: a republication between separate reads makes them differ
E1="unread"; E2="unread"; E3="unread"    # so the FAIL line can print
epochs_agree() {
	E1=$(q 1 map.term).$(q 1 map.seq)
	E2=$(q 2 map.term).$(q 2 map.seq)
	E3=$(q 3 map.term).$(q 3 map.seq)
	[ "$E1" = "$E2" ] && [ "$E2" = "$E3" ]
}

mk 1; mk 2; mk 3
start 1 && start 2 && start 3 || { echo "fleet did not start"; exit 1; }
# a joiner is CONNECTING until its links answer: up to 45 s for every node ready, seeing the others
st() { timeout 3 ./perfcli -h 127.0.55.$1 -p 1796$1 -a $SEC -q -j '{"method":"stats"}' 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
formed() { [ "$(st 1)" = "ready 2" ] && [ "$(st 2)" = "ready 2" ] && [ "$(st 3)" = "ready 2" ]; }
await 45 formed

# ---- 1. a term was claimed and a map published -------------------------
MT=0
for n in 1 2 3; do
	[ "$(q $n role)" = "master" ] && MASTER=$n
done
[ -n "${MASTER:-}" ] && ok "the fleet elected a master (node index $MASTER)" \
	|| { bad "no master"; echo "clmapwiretest: $pass passed, $((fail+1)) failed"; exit 1; }

T=$(q $MASTER term)
case "$T" in ''|ERR|0) bad "the master claimed no term (got '$T') - every map it issues would be unorderable against the next master's" ;;
	*) ok "the master claimed term $T" ;;
esac
P=$(q $MASTER map.published)
[ "${P:-0}" -ge 1 ] 2>/dev/null && ok "the master published a map ($P)" \
	|| bad "the master published no map (published=$P)"

# ---- 2 + 3. everyone adopts it, at the same epoch ----------------------
for n in 1 2 3; do
	await_map $n || bad "node $n never adopted a map"
done
# a node is READY on its own a heartbeat before the master hears it and maps it: poll, not one read
N="unread"
map_full() { N=$(q $MASTER map.nodes); [ "$N" = "3" ]; }
await 20 map_full && ok "the map names all 3 nodes" \
	|| bad "the map names $N node(s), expected 3"
if await 20 epochs_agree; then
	ok "every node holds the same epoch ($E1)"
else
	bad "epochs diverged: $E1 $E2 $E3"
fi

MID=$(q $MASTER node)
[ "$(q 1 map.master)" = "$MID" ] && ok "the map names the right master ($MID)" \
	|| bad "map master is $(q 1 map.master), the master says $MID"

# ---- 3b. ONLY the master publishes -------------------------------------
# enforced by where clmap_publish is called from: a new call site outside the master is caught here
NONM=0
for n in 1 2 3; do
	[ "$n" = "$MASTER" ] && continue
	P2=$(q $n map.published)
	[ "${P2:-0}" = "0" ] || NONM=$((NONM + 1))
done
[ "$NONM" = "0" ] && ok "no non-master published a map" \
	|| bad "$NONM non-master node(s) published a map - two publishers put
         two different maps at one epoch"

# ---- 4. nothing refused ------------------------------------------------
R=0
for n in 1 2 3; do R=$((R + $(q $n map.refused))); done
[ "$R" = "0" ] && ok "no map was refused" \
	|| bad "$R map(s) refused - the fleet disagrees about its own format"

# ---- 5. the term survives a restart ------------------------------------
BEFORE=$(q 2 term)
pkill -9 -f "$D/n2.conf"; sleep 1
: > "$D/n2.log"
start 2 || bad "node 2 did not restart"
sleep 3
AFTER=$(q 2 term)
[ "$AFTER" != "ERR" ] && [ "${AFTER:-0}" -ge "${BEFORE:-0}" ] 2>/dev/null \
	&& ok "the term survived a restart ($BEFORE -> $AFTER)" \
	|| bad "the term went backwards on restart ($BEFORE -> $AFTER) - a
         forgotten term can be reissued with different content"
grep -q "mastership term" "$D/n2.log" \
	&& ok "the restarted node logged the term it loaded" \
	|| bad "the restarted node said nothing about its term"

# ---- 6. re-publication moves the epoch forward -------------------------
S1=$(q $MASTER map.seq)
S2="unread"; RX="unread"                 # same reason as E1/E2/E3 above
seq_advanced() {
	S2=$(q $MASTER map.seq)
	[ "${S2:-0}" -gt "${S1:-0}" ] 2>/dev/null
}
if await 25 seq_advanced; then
	ok "re-publication advanced the epoch ($S1 -> $S2)"
else
	bad "the epoch did not advance ($S1 -> $S2) - a node that missed
         a datagram would never catch up"
fi

# ask the PEERS, roles re-derived every poll (a master's map.received is zero and mastership moves);
# the weakest peer decides, so a single node that stops adopting is caught
RXLOW="unread"; RXN="?"
rx_enough() {
	rx_low=""; rx_node=""
	for n in 1 2 3; do
		set -- $(rr $n)
		[ "$1" = "master" ] && continue
		if [ "$1" = "ERR" ]; then
			rx_low=""                  # unreadable: not a verdict
			break
		fi
		v=$2
		case "$v" in ''|*[!0-9]*) v=0 ;; esac
		if [ -z "$rx_low" ] || [ "$v" -lt "$rx_low" ]; then
			rx_low=$v; rx_node=$n
		fi
	done
	RXLOW=${rx_low:-unread}; RXN=${rx_node:-?}
	[ -n "$rx_low" ] && [ "$rx_low" -ge 2 ] 2>/dev/null
}
if await 25 rx_enough; then
	ok "peers keep receiving maps (lowest $RXLOW, node index $RXN)"
elif [ "$RXLOW" = "unread" ]; then
	bad "a peer never answered a stats query - cannot tell whether maps
         are still arriving"
else
	bad "peer at node index $RXN received only $RXLOW map(s)"
fi

echo "clmapwiretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
