#!/bin/sh
# shardswitchtest.sh — ownership now comes from the map (wiring step D).
#
# The failure this is built around is not a crash, it is a SILENT
# FALLBACK: pc_shard_owner consults the map and falls back to the
# fleet's own HRW when it cannot.  If the map never arrives, every
# existing test still passes - because the fallback is the code that
# worked before.  So the first thing asserted is that map-based
# placement is actually being used.
#
#  1. the map is used, and the HRW fallback stops being reached
#  2. every node agrees on where a key lives.  Two nodes disagreeing
#     about an owner is split ownership, which puts one key in two
#     places - the thing the whole control plane exists to prevent
#  3. keys written through one node are readable through every other
#  4. it survives a node leaving: the map republishes, ownership moves
#     once, and reads still resolve
# Usage: test/shardswitchtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=sw-client-secret
D=$(mktemp -d /var/tmp/pcsw.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17941 17942 17943; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "shardswitchtest: port $pf already bound" >&2; exit 1; }
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
cluster = sw-cluster-secret
[listen]
tcp = 127.0.57.$1:1794$1
[wal]
dir = $D/s$1
segment_mb = 8
probe = no
save = off
[cluster]
multicast = 239.255.77.195:17295
advertise = 127.0.57.$1
mode = shard
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
cli() { n=$1; shift; ./perfcli -h 127.0.57.$n -p 1794$n -a $SEC "$@" 2>/dev/null; }
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
put() { cli $1 -j "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"$2\",\"value\":\"$3\"}}" >/dev/null; }
get() { cli $1 -j "{\"method\":\"get\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}" \
	| python3 -c 'import json,sys
try:
    r = json.load(sys.stdin)
    print(r.get("value") if r.get("found") else "-")
except Exception: print("ERR")'; }

mk 1; mk 2; mk 3
start 1 && start 2 && start 3 || { echo "fleet did not start"; exit 1; }
sleep 8

# ---- 1. the map is ACTUALLY used --------------------------------------
for i in 1 2 3 4 5 6 7 8 9 10; do put 1 "sk$i" "v$i"; done
sleep 1
U=$(q 1 map.usable)
[ "$U" = "True" ] && ok "the map caught up with what this node sees as live" \
	|| bad "the map never became usable ($U) - placement would sit on
         the liveness fallback for ever, which is not a switchover"
PM=$(q 1 map.place_map)
PH=$(q 1 map.place_hrw)
[ "${PM:-0}" -gt 0 ] 2>/dev/null \
	&& ok "ownership came from the map ($PM decisions)" \
	|| bad "the map decided nothing (place_map=$PM, place_hrw=$PH) - the
         switchover fell back to the old path and every other test here
         would pass anyway"

# after the map has settled, the fallback must stop being reached
PH1=$(q 1 map.place_hrw)
for i in 11 12 13 14 15 16 17 18 19 20; do put 1 "sk$i" "v$i"; done
sleep 1
PH2=$(q 1 map.place_hrw)
[ "${PH2:-0}" = "${PH1:-0}" ] \
	&& ok "the HRW fallback is no longer reached ($PH2)" \
	|| bad "the fallback is still being used ($PH1 -> $PH2) - the map is
         not answering for every key"

# ---- 2. every node agrees on where a key lives ------------------------
DIS=0
for i in 1 2 3 4 5 6 7 8 9 10; do
	A=$(get 1 "sk$i"); B=$(get 2 "sk$i"); C2=$(get 3 "sk$i")
	[ "$A" = "$B" ] && [ "$B" = "$C2" ] || DIS=$((DIS + 1))
done
[ "$DIS" = "0" ] \
	&& ok "all three nodes resolve every key identically" \
	|| bad "$DIS of 10 keys resolved differently depending on which node
         was asked - that is split ownership"

# ---- 3. written anywhere, readable everywhere -------------------------
put 2 crosskey crossval
sleep 1
[ "$(get 1 crosskey)" = "crossval" ] && [ "$(get 3 crosskey)" = "crossval" ] \
	&& ok "a key written through one node reads through the others" \
	|| bad "a key written through node 2 is not readable elsewhere
         (n1=$(get 1 crosskey) n3=$(get 3 crosskey))"

# ---- 4. a node leaves: the map moves ownership once -------------------
# Compared as an EPOCH, term first.  Raw seq only orders within a term:
# if the node killed happens to be the master, a standby promotes under
# a NEW term and the sequence legitimately restarts at 1, which reads as
# the map going backwards when it has in fact moved forwards.
epoch_gt() {  # epoch_gt T1 S1 T2 S2  -> true when (T1,S1) > (T2,S2)
	[ "${1:-0}" -gt "${3:-0}" ] 2>/dev/null && return 0
	[ "${1:-0}" = "${3:-0}" ] && [ "${2:-0}" -gt "${4:-0}" ] 2>/dev/null
}
surv() {	# first live node of 1,2 - a break-loop, not `| head -1`:
	# head exits after its line, and the next iteration's echo then
	# writes a closed pipe - one "echo: I/O error" per wait tick all
	# over slow-runner CI logs (the value was correct throughout)
	for n in 1 2; do
		pgrep -f "$D/n$n.conf" >/dev/null && { echo $n; return; }
	done
}
T1=$(q 1 map.term); SEQ1=$(q 1 map.seq)
pkill -9 -f "$D/n3.conf"
# wait for the fleet to purge it - PEER_PURGE_MS, plus a promotion if the
# node that died was the master
i=0
while [ $i -lt 40 ]; do
	SURV=$(surv)
	[ "$(q $SURV map.nodes)" = "2" ] && break
	sleep 1; i=$((i+1))
done
SURV=$(surv)
# an empty SURV is not a timing problem to ride out - both survivors
# are gone (daemons died, or something external killed them); say so
# instead of letting set -u abort mid-helper with "parameter not set"
[ -n "${SURV:-}" ] || {
	echo "shardswitchtest: ABORT - neither node 1 nor 2 is alive after the loss" >&2
	exit 1; }
T2=$(q $SURV map.term); SEQ2=$(q $SURV map.seq)
epoch_gt "$T2" "$SEQ2" "$T1" "$SEQ1" \
	&& ok "the map republished after a node left ($T1.$SEQ1 -> $T2.$SEQ2)" \
	|| bad "the map did not move after a node left ($T1.$SEQ1 -> $T2.$SEQ2) -
         ownership would keep pointing at a node that is gone"
N=$(q $SURV map.nodes)
[ "${N:-0}" = "2" ] && ok "the map now names 2 nodes" \
	|| bad "the map still names $N node(s) after one left"
# Write-then-read with a deadline, not sleep-and-hope: right after the
# loss the survivors are mid-housekeeping, and on a loaded host a fixed
# 1s window (plus the client's own timeout) measures the HOST, not the
# daemon.  The put is re-issued each tick - its reply is discarded by
# the helper, so a legal settling-window refusal would silently strand
# a single attempt.
i=0
while [ $i -lt 20 ]; do
	put $SURV afterloss stillhere
	[ "$(get $SURV afterloss)" = "stillhere" ] && break
	sleep 1; i=$((i+1))
done
[ "$(get $SURV afterloss)" = "stillhere" ] \
	&& ok "writes still resolve after the loss" \
	|| bad "a write after the loss was not readable ($(get $SURV afterloss))"

echo "shardswitchtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
