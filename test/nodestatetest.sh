#!/bin/sh
# nodestatetest.sh — task B1: what state is this node in, and can its
# data be trusted yet?
#
# The cluster already had a ROLE (joining/member/master).  That answers
# "what authority does this node have in the membership", not "is its
# data usable" - a master can be perfectly authoritative and still be
# settling its keyspace.  B1 adds the second axis.
#
#  1. a node with no cluster is READY as soon as it is up - there is
#     nothing to join, and it must not sit in a state that a later gate
#     would refuse clients in
#  2. a clustered node reaches READY, and says so in stats
#  3. every member's state is visible from any node (members verb), not
#     just its own
#  4. the transitions are LOGGED, so a node stuck somewhere can be
#     diagnosed from the log alone
#  5. a peer's state is learned from its heartbeat, not assumed
# Usage: test/nodestatetest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=ns-client-secret
D=$(mktemp -d /var/tmp/pcns.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17991 17992 17993; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "nodestatetest: port $pf already bound" >&2; exit 1; }
done

# $1 = index, $2 = "solo" for no [cluster] section
mk() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ns-cluster-secret
[listen]
tcp = 127.0.54.$1:1799$1
EOF
	[ "${2:-}" = "solo" ] || cat >> "$D/n$1.conf" <<EOF
[cluster]
multicast = 239.255.77.181:17281
advertise = 127.0.54.$1
mode = store
collections = c
EOF
	cat >> "$D/n$1.conf" <<EOF
[collection c]
buckets_log2 = 12
EOF
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
cli() { n=$1; shift; ./perfcli -h 127.0.54.$n -p 1799$n -a $SEC "$@" 2>/dev/null; }
state() {
	cli $1 -j '{"method":"stats"}' | python3 -c 'import json,sys
try: print(json.load(sys.stdin)["state"])
except Exception: print("ERR")'
}
# every member as node $1 sees the fleet: "node=state node=state ..."
memstates() {
	cli $1 -j '{"method":"members"}' | python3 -c 'import json,sys
try:
    d = json.load(sys.stdin)
    ms = d["members"] if isinstance(d, dict) and "members" in d else d
    print(" ".join("%s=%s" % (m["node"], m.get("state","MISSING"))
                   for m in sorted(ms, key=lambda x: x["node"])))
except Exception as e: print("ERR")'
}
await_state() {
	i=0
	while [ $i -lt 80 ]; do
		[ "$(state $1)" = "$2" ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}

# ---- 1. no cluster: READY immediately ----------------------------------
mk 3 solo
start 3 || { echo "solo node did not start"; exit 1; }
S=$(state 3)
[ "$S" = "ready" ] && ok "a node with no cluster is ready at once" \
	|| bad "solo node is '$S', not ready - a readiness gate would refuse
         clients on a node that has nothing to wait for"
pkill -9 -f "$D/n3.conf"; sleep 1

# ---- 2 + 3. a clustered fleet settles and everyone can see it ----------
mk 1; mk 2
start 1 && start 2 || { echo "fleet did not start"; exit 1; }
await_state 1 ready && ok "a clustered node reaches ready" \
	|| bad "node 1 never reached ready (stuck in $(state 1))"
await_state 2 ready || bad "node 2 never reached ready (stuck in $(state 2))"

# both must be visible, with a state, from EITHER node.  Reaching ready
# is NOT the same as having seen every peer: a founding master is ready
# at once, and the client ports members[] needs arrive on the next
# heartbeat.  Wait for the membership, do not read it once and assume.
await_members() {
	amn=$1                         # set -- below CLOBBERS $1
	i=0
	while [ $i -lt 80 ]; do
		set -- $(memstates $amn)
		[ $# -ge 2 ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}
await_members 1 || bad "node 1 never saw a second member"
await_members 2 || bad "node 2 never saw a second member"
M1=$(memstates 1); M2=$(memstates 2)
case "$M1" in
	*MISSING*|ERR|"") bad "members from node 1 carry no state: '$M1'" ;;
	*) case "$M1" in
		*=ready*) ok "every member's state is visible from node 1 ($M1)" ;;
		*) bad "node 1 sees no ready member: '$M1'" ;;
	   esac ;;
esac
case "$M2" in
	*=ready*) ok "and from node 2 ($M2)" ;;
	*) bad "node 2 sees no ready member: '$M2'" ;;
esac

# node 1 must see node 2 as ready - that state came off the WIRE, not
# from a local assumption
echo "$M1" | grep -q "^[0-9]*=ready [0-9]*=ready$" \
	&& ok "a peer's state is learned from its heartbeat" \
	|| bad "node 1 does not see both members ready: '$M1'"

# ---- 4. the transitions are in the log ---------------------------------
grep -q "node state .* -> ready" "$D/n1.log" \
	&& ok "the transition to ready is logged" \
	|| bad "no state transition in the log - a stuck node could not be
         diagnosed from it"
# JOINING is no longer a NODE state - it is a membership question, and
# PC_ROLE_JOINING already answers it.  What must hold now is that the
# node axis never reports it.
! grep -q "node state .* -> joining" "$D/n1.log" \
	&& ok "joining is not reported on the node axis" \
	|| bad "joining is still a node state - it duplicates the role axis"
! grep -q "node state .* -> reconciling" "$D/n1.log" \
	&& ok "reconciling is not reported either" \
	|| bad "reconciling is still a node state"

echo "nodestatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
