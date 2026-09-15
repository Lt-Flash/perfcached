#!/bin/sh
# failovertest.sh — harness for the cluster-aware client (task S34).
# Runs a three-node store cluster, lets failovertest connect with
# pre-warmed standbys, KILLS the node it is using on cue, and checks
# the client rode it out.  Then two policy legs the C program cannot do
# alone: independent clients must not all land on one node, and a
# non-idempotent verb must not be silently replayed.
# Usage: test/failovertest.sh [./perfcached] [./failovertest]
set -u

BIN=${1:-./perfcached}
FT=${2:-./failovertest}
D=$(mktemp -d /var/tmp/pcfo.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3" "$S1" "$S2" "$S3" "$S4" "$MBPID"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done; rm -rf "$D"' EXIT TERM INT
S1= S2= S3= S4= MBPID=
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }
SECRET=fo-client-secret

node() { # node <id> <port>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = $SECRET
cluster = fo-cluster-secret
[listen]
tcp = 127.0.9.$1:$2
[cluster]
multicast = 239.255.77.76:17176
advertise = 127.0.9.$1
pull_timeout_ms = 400
mode = store
collections = c
EOF
}
start() { # start <id> <pidvar>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "$2=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

node 1 17501
node 2 17502
node 3 17503
start 1 P1
start 2 P2
start 3 P3
sleep 4

# node id -> pid, learned from each daemon's own view
# (node ids are not queryable here - the daemons are encrypted and the
# harness speaks no Noise; the client names the PORT it is using, which
# it knows from the member list)

# ---- the main leg: run the client, kill the node it names ----------
"$FT" 127.0.9.1 17501 17502 17503 "$SECRET" > "$D/ft.out" 2>&1 &
FTPID=$!
# S104: first cue - the client names a STANDBY's port; kill that node.
# The client then proves the idle ping finds it while the active serves.
i=0
SKILLED=
while [ $i -lt 100 ]; do
	SK=$(grep -m1 "^KILL-SPARE-PORT " "$D/ft.out" 2>/dev/null | awk '{print $2}')
	if [ -n "$SK" ]; then
		case "$SK" in
			17501) kill -9 $P1 2>/dev/null; P1=; SKILLED=1;;
			17502) kill -9 $P2 2>/dev/null; P2=; SKILLED=1;;
			17503) kill -9 $P3 2>/dev/null; P3=; SKILLED=1;;
		esac
		break
	fi
	sleep 0.1; i=$((i+1))
done
[ -n "$SKILLED" ] && ok || bad "client never named a standby to kill"
# wait for the KILL-NODE line, then kill that node
i=0
KILLED=
while [ $i -lt 100 ]; do
	KN=$(grep -m1 "^KILL-PORT " "$D/ft.out" 2>/dev/null | awk '{print $2}')
	if [ -n "$KN" ]; then
		case "$KN" in
			17501) kill -9 $P1 2>/dev/null; P1=; KILLED=1;;
			17502) kill -9 $P2 2>/dev/null; P2=; KILLED=1;;
			17503) kill -9 $P3 2>/dev/null; P3=; KILLED=1;;
		esac
		break
	fi
	sleep 0.1; i=$((i+1))
done
[ -n "$KILLED" ] && ok || bad "client never named a node to kill"
wait $FTPID 2>/dev/null
FTRC=$?
sed -n 's/^ok: /  ok: /p;s/^FAIL: /  FAIL: /p' "$D/ft.out"
N_OK=$(grep -c "^ok: " "$D/ft.out" 2>/dev/null); N_OK=${N_OK:-0}
N_BAD=$(grep -c "^FAIL: " "$D/ft.out" 2>/dev/null); N_BAD=${N_BAD:-0}
case "$N_OK" in ''|*[!0-9]*) N_OK=0;; esac
case "$N_BAD" in ''|*[!0-9]*) N_BAD=0;; esac
pass=$((pass + N_OK)); fail=$((fail + N_BAD))
[ "$FTRC" = "0" ] || echo "  (failovertest exit $FTRC)"

# ---- a second, healthy fleet for the legs below ----------------------
# The main leg has just killed TWO of its three nodes (the active on
# KILL-PORT, a standby on KILL-SPARE-PORT), so it cannot host a spread
# test: round-robin over one usable member puts every client on it -
# which is exactly what rc15's check job reported once the second kill
# existed.  Nodes 4-6 are the routing leg's shard fleet; started here so
# the spread leg has three live members to spread over, and the 4 s
# lets them form a cluster before anyone asks for its member list.
node 4 17504
node 5 17505
node 6 17506
sed -i 's/^mode = store$/mode = shard/' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/239.255.77.76:17176/239.255.77.77:17177/' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/^advertise = 127.0.9./advertise = 127.0.10./' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/^tcp = 127.0.9./tcp = 127.0.10./' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
S1= S2= S3=
start 4 S1; start 5 S2; start 6 S3
sleep 4

# ---- policy leg: independent clients must not all pick one node ----
# round-robin picks a random start per client, so over many clients the
# active node should not be constant
cat > "$D/spread.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perfd.h"
int main(int argc, char **argv)
{
	const char *sec[2];
	perfd_opts o;
	int i;

	if (argc < 4) return 2;
	sec[0] = argv[3]; sec[1] = NULL;
	for (i = 0; i < 12; i++) {
		perfd_t *p;

		memset(&o, 0, sizeof o);
		o.secrets = sec;
		o.spares = -1;
		o.policy = PERFD_POLICY_ROUND_ROBIN;
		p = perfd_connect(argv[1], atoi(argv[2]), &o);
		if (!p) { printf("-1\n"); continue; }
		printf("%d\n", perfd_active_node(p));
		perfd_free(p);
	}
	return 0;
}
EOF
# $SAN, or this leg cannot link under check-asan.  libperfd.a is built
# with the sanitizer when SAN is set, so a plain cc leaves every ASan
# runtime symbol undefined and the link dies - which this used to
# swallow as a "skipped" line.  It ran green in CI for exactly as long
# as nobody read the log: the leg was never executing under ASan, which
# is the configuration most likely to catch a client-side bug.  make
# exports a command-line SAN into recipe shells, so it is simply here
# for the taking.
#
# And a compile failure is now a FAILURE, not a skip.  Only a genuinely
# absent compiler is a legitimate reason to not run this - anything else
# is a broken leg reporting success by not looking.
if ! command -v cc >/dev/null 2>&1; then
	echo "  (spread leg skipped: no cc on this host)"
elif cc ${SAN:-} -O1 -Ilib -o "$D/spread" "$D/spread.c" libperfd.a \
		-lpthread -lsodium 2>"$D/cc.log"; then
	DISTINCT=$("$D/spread" 127.0.10.5 17505 "$SECRET" 2>/dev/null | sort -u | grep -vc '^-1$')
	[ "${DISTINCT:-0}" -ge 2 ] && ok \
		|| bad "round-robin put 12 clients on ${DISTINCT:-0} node(s)"
else
	bad "the spread leg did not BUILD, so round-robin spread went
	     untested: $(tail -1 "$D/cc.log")"
fi

# ---- S35: per-key routing removes the forward hop -------------------
# The proof is the DAEMONS' counters, not the client's opinion: on a
# shard cluster a routed client should make the fleet forward (almost)
# nothing, and the same load with routing OFF should make it forward a
# lot.  The measurement must be able to show the difference, or it is
# not a measurement.  (Nodes 4-6 were started above, for the spread leg.)
# the client library is the only thing here that speaks Noise, so the
# counters are read through it: perfcli reports stats as JSON
# A node that cannot be READ must not read as a node that forwarded
# NOTHING.  Measured 2026-09-02: a tree built with `make perfcached
# failovertest` and no perfcli made every one of these return empty,
# ${V:-0} turned that into 0, and the control leg reported "forwarded
# only 0 - the counter cannot show the difference".  That looks exactly
# like a routing defect and is a missing binary.  Same family as the
# spread leg above: an instrument that reports a value by not looking.
fleet_fwd() {
	F=0
	for hp in "127.0.10.4 17504" "127.0.10.5 17505" "127.0.10.6 17506"; do
		set -- $hp
		V=$(./perfcli -h "$1" -p "$2" -a "$SECRET" stats 2>/dev/null \
			| tr -d ' \n' | sed -n 's/.*"fwd_sent":\([0-9]*\).*/\1/p')
		# NOT ${V:-0}: a node that did not answer has not told us it
		# forwarded nothing.  `exit` here would only leave the command
		# substitution, so record it and let the caller decide.
		[ -z "$V" ] && { echo "$1:$2" >> "$D/fwd.unreadable"; continue; }
		F=$((F + V))
	done
	echo "$F"
}
# CONTROL first: the same load with routing OFF must make the counter
# climb, or "0 forwards" later proves nothing about routing
# The tool check goes HERE, not inside fleet_fwd: there it sits in a
# command substitution, where `exit` leaves only the subshell and the
# suite carries on with a fabricated 0 - which is exactly how a missing
# perfcli spent an afternoon looking like a routing defect.
rm -f "$D/fwd.unreadable"
if [ ! -x ./perfcli ]; then
	bad "./perfcli is missing, so the S35 forward counters cannot be read
	     at all - build it (make perfcli).  Refusing to run the leg and
	     report 0 forwards, which is indistinguishable from a routing
	     failure."
	SKIP_S35=1
fi
B0=$(fleet_fwd)
"$FT" 127.0.10.4 17504 17505 17506 "$SECRET" noroute > "$D/nrt.out" 2>&1
B1=$(fleet_fwd)
UNROUTED=$((B1 - B0))
echo "  fleet fwd_sent for 400 UNROUTED ops: $UNROUTED"
if [ -s "$D/fwd.unreadable" ]; then
	bad "node(s) $(tr '\n' ' ' < "$D/fwd.unreadable")did not report
	     fwd_sent - the $UNROUTED above is a partial sum and must not be
	     read as a routing result"
elif [ "${SKIP_S35:-0}" = 1 ]; then
	:                                  # already reported above
elif [ "$UNROUTED" -gt 50 ]; then
	ok
else
	bad "control leg forwarded only $UNROUTED - the counter cannot show the difference"
fi
sed -n 's/^ok: /  ok: /p;s/^FAIL: /  FAIL: /p' "$D/nrt.out"
C_OK=$(grep -c "^ok: " "$D/nrt.out" 2>/dev/null); C_OK=${C_OK:-0}
C_BAD=$(grep -c "^FAIL: " "$D/nrt.out" 2>/dev/null); C_BAD=${C_BAD:-0}
case "$C_OK" in ''|*[!0-9]*) C_OK=0;; esac
case "$C_BAD" in ''|*[!0-9]*) C_BAD=0;; esac
pass=$((pass + C_OK)); fail=$((fail + C_BAD))

B2=$(fleet_fwd)
"$FT" 127.0.10.4 17504 17505 17506 "$SECRET" route > "$D/rt.out" 2>&1
sed -n 's/^ok: /  ok: /p;s/^FAIL: /  FAIL: /p' "$D/rt.out"
R_OK=$(grep -c "^ok: " "$D/rt.out" 2>/dev/null); R_OK=${R_OK:-0}
R_BAD=$(grep -c "^FAIL: " "$D/rt.out" 2>/dev/null); R_BAD=${R_BAD:-0}
case "$R_OK" in ''|*[!0-9]*) R_OK=0;; esac
case "$R_BAD" in ''|*[!0-9]*) R_BAD=0;; esac
pass=$((pass + R_OK)); fail=$((fail + R_BAD))
FWD=$(( $(fleet_fwd) - B2 ))
echo "  fleet fwd_sent for 400 ROUTED ops:   $FWD  (unrouted: $UNROUTED)"
if [ "$FWD" -le 20 ]; then ok; else
	bad "routed load still forwarded $FWD times (routing not effective)"
fi
MISSED=$(sed -n 's/^ROUTE-MISSED //p' "$D/rt.out")
[ "${MISSED:-0}" -eq 0 ] && ok || bad "routing missed ${MISSED} owners"


# ---- S104: the whole fleet restarts under a handle; the seed stays down
# The client (mode "restart") holds nodes 4-6 and names three cues:
# RESTART-ALL (kill and restart all three), KILL-ALL (kill all three),
# START-OTHERS (start 5 and 6 only - 4, the seed, stays down).  What it
# asserts: the second operation after the restart succeeds, the handle
# fails fast with the fleet gone, and perfd_redial reaches the fleet
# through a learned member.
cue() { # cue <file> <word> <secs>: wait for a cue line
	i=0
	while [ $i -lt $(( $3 * 10 )) ]; do
		grep -q "^$2\$" "$1" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
"$FT" 127.0.10.4 17504 17505 17506 "$SECRET" restart > "$D/rs.out" 2>&1 &
RSPID=$!
if cue "$D/rs.out" RESTART-ALL 20; then
	for pv in S1 S2 S3; do eval "kill -9 \$$pv 2>/dev/null"; done
	sleep 1
	start 4 S1; start 5 S2; start 6 S3
	ok
else
	bad "the restart client never asked for the restart: $(tail -2 "$D/rs.out" | tr '\n' ' ')"
fi
if cue "$D/rs.out" KILL-ALL 40; then
	for pv in S1 S2 S3; do eval "kill -9 \$$pv 2>/dev/null"; done
	S1= S2= S3=
	ok
else
	bad "the restart client never asked for the kill: $(tail -2 "$D/rs.out" | tr '\n' ' ')"
fi
if cue "$D/rs.out" START-OTHERS 20; then
	start 5 S2; start 6 S3
	ok
else
	bad "the restart client never asked for the others: $(tail -2 "$D/rs.out" | tr '\n' ' ')"
fi
cue "$D/rs.out" DONE 30 || bad "the restart client did not finish: $(tail -2 "$D/rs.out" | tr '\n' ' ')"
wait $RSPID 2>/dev/null
sed -n 's/^ok: /  ok: /p;s/^FAIL: /  FAIL: /p' "$D/rs.out"
X_OK=$(grep -c "^ok: " "$D/rs.out" 2>/dev/null); X_OK=${X_OK:-0}
X_BAD=$(grep -c "^FAIL: " "$D/rs.out" 2>/dev/null); X_BAD=${X_BAD:-0}
case "$X_OK" in ''|*[!0-9]*) X_OK=0;; esac
case "$X_BAD" in ''|*[!0-9]*) X_BAD=0;; esac
pass=$((pass + X_OK)); fail=$((fail + X_BAD))

# ---- S107: a member bounced, a node added, a node expelled ------------
# The restart leg left node 4 down: bring it back so the client holds a
# whole fleet.  Cues: BOUNCE-PORT (kill -9 and restart at once - inside
# the purge window, so the fleet keeps it as a member and the client
# must recover it in the background), ADD-NODE (node 7 joins the shard
# fleet), EXPEL-PORT (a clean stop: GOODBYE, expelled at once).
start 4 S1
sleep 3
node 7 17507
sed -i 's/^mode = store$/mode = shard/' "$D/n7.conf"
sed -i 's/239.255.77.76:17176/239.255.77.77:17177/' "$D/n7.conf"
sed -i 's/^advertise = 127.0.9./advertise = 127.0.10./' "$D/n7.conf"
sed -i 's/^tcp = 127.0.9./tcp = 127.0.10./' "$D/n7.conf"
S4=
pidvar() { case "$1" in 17504) echo S1;; 17505) echo S2;; 17506) echo S3;; 17507) echo S4;; esac; }
nodeof() { case "$1" in 17504) echo 4;; 17505) echo 5;; 17506) echo 6;; 17507) echo 7;; esac; }
"$FT" 127.0.10.4 17504 17505 17506 "$SECRET" member > "$D/mb.out" 2>&1 &
MBPID=$!
if cue "$D/mb.out" "BOUNCE-PORT [0-9]*" 20; then
	BP=$(sed -n 's/^BOUNCE-PORT //p' "$D/mb.out" | head -1)
	PV=$(pidvar "$BP"); NN=$(nodeof "$BP")
	eval "kill -9 \$$PV 2>/dev/null"
	start "$NN" "$PV"
	ok
else
	bad "the member client never asked for the bounce: $(tail -2 "$D/mb.out" | tr '\n' ' ')"
fi
if cue "$D/mb.out" ADD-NODE 40; then
	start 7 S4
	ok
else
	bad "the member client never asked for the new node: $(tail -2 "$D/mb.out" | tr '\n' ' ')"
fi
if cue "$D/mb.out" "EXPEL-PORT [0-9]*" 20; then
	kill -TERM $S4 2>/dev/null; wait $S4 2>/dev/null; S4=
	ok
else
	bad "the member client never asked for the expulsion: $(tail -2 "$D/mb.out" | tr '\n' ' ')"
fi
cue "$D/mb.out" DONE 30 || bad "the member client did not finish: $(tail -2 "$D/mb.out" | tr '\n' ' ')"
wait $MBPID 2>/dev/null
sed -n 's/^ok: /  ok: /p;s/^FAIL: /  FAIL: /p' "$D/mb.out"
M_OK=$(grep -c "^ok: " "$D/mb.out" 2>/dev/null); M_OK=${M_OK:-0}
M_BAD=$(grep -c "^FAIL: " "$D/mb.out" 2>/dev/null); M_BAD=${M_BAD:-0}
case "$M_OK" in ''|*[!0-9]*) M_OK=0;; esac
case "$M_BAD" in ''|*[!0-9]*) M_BAD=0;; esac
pass=$((pass + M_OK)); fail=$((fail + M_BAD))

# the legs above leave nodes 4-6 up and 7 gone
for v in "$S1" "$S2" "$S3" "$S4"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; done
echo "failovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
