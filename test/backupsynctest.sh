#!/bin/sh
# backupsynctest.sh — the standby, and what it is for.
#
# A backup that is only a NAME buys nothing: the point is that when the
# master dies, the node taking over already holds the state the fleet
# cannot reconstruct.  The piece that matters is the IDENTITY HISTORY -
# only a master ever learns which identities this cluster has seen, so a
# promotion without it calls every returning node NEW, backfilling nodes
# that only needed reconciling.
#
#  1. a standby is designated, and every node agrees who it is
#  2. it holds the control state; a plain member does not
#  3. publications are acknowledged BEFORE they are broadcast
#  4. losing the STANDBY degrades to picking another one, not to a
#     wedged control plane - which is the risk of gating publication on
#     an ack at all
#  5. losing the MASTER promotes the standby WITH the history, and the
#     new master claims a HIGHER term - never a reused one
# Usage: test/backupsynctest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=bs-client-secret
D=$(mktemp -d /var/tmp/pcbsy.XXXXXX)
trap 'pkill -9 -f "/var/tmp/[p]cbsy" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17921 17922 17923; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "backupsynctest: port $pf already bound" >&2; exit 1; }
done

mk() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = bs-cluster-secret
[listen]
tcp = 127.0.70.$1:1792$1
[cluster]
multicast = 239.255.77.221:17321
advertise = 127.0.70.$1
mode = store
eager = 1
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
q() {
	./perfcli -h 127.0.70.$1 -p 1792$1 -a $SEC -j '{"method":"stats"}' \
		2>/dev/null | python3 -c '
import json,sys
p = sys.argv[1].split(".")
try:
    o = json.load(sys.stdin)["cluster"]
    for k in p: o = o[k]
    print(o)
except Exception: print("ERR")' "$2"
}
alive() { pgrep -f "$D/n$1.conf" >/dev/null 2>&1; }
# which local index is currently master / the designated backup
idx_of_node() {
	for n in 1 2 3; do
		alive $n || continue
		[ "$(q $n node)" = "$1" ] && { echo $n; return; }
	done
}

mk 1; mk 2; mk 3
start 1 && start 2 && start 3 || { echo "fleet did not start"; exit 1; }
sleep 10

# ---- 1. a standby is designated and everyone agrees -------------------
B1=$(q 1 map.backup); B2=$(q 2 map.backup); B3=$(q 3 map.backup)
[ "$B1" = "$B2" ] && [ "$B2" = "$B3" ] && [ "${B1:-0}" != "0" ] \
	&& ok "every node names the same standby ($B1)" \
	|| bad "nodes disagree about the standby: $B1 $B2 $B3 - a second
         failure in that window would have no defined answer"

# ---- 2. the standby holds state; a member does not --------------------
BI=$(idx_of_node "$B1")
[ -n "${BI:-}" ] && [ "$(q $BI sync.held)" = "True" ] \
	&& ok "the standby holds the control state" \
	|| bad "the standby (index ${BI:-?}) holds nothing - a promotion
         would start from scratch"
IDS=$(q $BI sync.held_identities)
[ "${IDS:-0}" -ge 3 ] 2>/dev/null \
	&& ok "it holds the identity history ($IDS identities)" \
	|| bad "the standby holds $IDS identities - without them a promoted
         master calls every returning node new"
for n in 1 2 3; do
	[ "$n" = "$BI" ] && continue
	[ "$(q $n role)" = "master" ] && continue
	[ "$(q $n sync.held)" = "False" ] && ok "a plain member holds nothing" \
		|| bad "a non-standby is holding control state"
	break
done

# ---- 3. acked before broadcast ----------------------------------------
MI=$(for n in 1 2 3; do [ "$(q $n role)" = "master" ] && echo $n; done)
SENT=$(q $MI sync.sent); ACKED=$(q $MI sync.acked)
[ "${SENT:-0}" -gt 0 ] 2>/dev/null && [ "${ACKED:-0}" -gt 0 ] 2>/dev/null \
	&& ok "the master syncs and is acknowledged (sent $SENT, acked $ACKED)" \
	|| bad "no acknowledged sync (sent $SENT, acked $ACKED) - publication
         is not actually going past the standby"

# ---- 4. losing the STANDBY must not wedge publishing ------------------
SEQ_BEFORE=$(q $MI map.seq)
pkill -9 -f "$D/n$BI.conf"; sleep 14
SEQ_AFTER=$(q $MI map.seq)
[ "${SEQ_AFTER:-0}" -gt "${SEQ_BEFORE:-0}" ] 2>/dev/null \
	&& ok "publishing continued after losing the standby ($SEQ_BEFORE -> $SEQ_AFTER)" \
	|| bad "the epoch stalled at $SEQ_AFTER after the standby died - gating
         on an ack has wedged the control plane"
NB=$(q $MI map.backup)
[ "${NB:-0}" != "0" ] && [ "$NB" != "$B1" ] \
	&& ok "a new standby was designated ($B1 -> $NB)" \
	|| bad "no new standby after the old one died (still $NB)"

# ---- 5. losing the MASTER promotes the standby WITH the history -------
TERM_BEFORE=$(q $MI term)
pkill -9 -f "$D/n$MI.conf"; sleep 18
NM=$(for n in 1 2 3; do alive $n && [ "$(q $n role)" = "master" ] && echo $n; done)
[ -n "${NM:-}" ] && ok "the standby promoted to master" \
	|| { bad "nobody promoted after the master died"; \
	     echo "backupsynctest: $pass passed, $((fail)) failed"; exit 1; }
OWN=$(q $NM identities_seen)
[ "${OWN:-0}" -ge 3 ] 2>/dev/null \
	&& ok "it promoted holding the identity history ($OWN identities)" \
	|| bad "the new master remembers $OWN identities - it started blank,
         and every returning node will look new to it"
TERM_AFTER=$(q $NM term)
[ "${TERM_AFTER:-0}" -gt "${TERM_BEFORE:-0}" ] 2>/dev/null \
	&& ok "the new master claimed a higher term ($TERM_BEFORE -> $TERM_AFTER)" \
	|| bad "the new master's term is $TERM_AFTER against $TERM_BEFORE - a
         reused term makes two masters' maps unorderable"
grep -q "promoted holding the previous master" "$D/n$NM.log" \
	&& ok "and said so in its log" \
	|| bad "the promotion did not report inheriting state"

echo "backupsynctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
