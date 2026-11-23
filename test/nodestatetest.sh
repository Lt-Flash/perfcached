#!/bin/sh
# nodestatetest.sh - the node state axis (is its data usable yet), beside the membership role: READY at
# once solo, visible for every member from any node, learned from heartbeats, every transition logged.
set -u

BIN=${1:-./perfcached}
SEC=ns-client-secret
D=$(mktemp -d /var/tmp/pcns.XXXXXX)
# daemons die by the pids this script started, each checked against its command line so a reused pid
# never hits a stranger; the name pattern is only a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
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

# both visible, with a state, from EITHER node: a founding master is ready at once, and the ports
# members[] needs arrive on the next heartbeat - so wait for the membership
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
# JOINING is a membership question (PC_ROLE_JOINING), never a node state
! grep -q "node state .* -> joining" "$D/n1.log" \
	&& ok "joining is not reported on the node axis" \
	|| bad "joining is still a node state - it duplicates the role axis"
! grep -q "node state .* -> reconciling" "$D/n1.log" \
	&& ok "reconciling is not reported either" \
	|| bad "reconciling is still a node state"

echo "nodestatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
