#!/bin/sh
# failovertest.sh - the cluster-aware client rides out killed, restarted and expelled nodes, spreads
# independent clients and routes per key.  Usage: test/failovertest.sh [./perfcached] [./failovertest]
set -u

BIN=${1:-./perfcached}
FT=${2:-./failovertest}
D=$(mktemp -d /var/tmp/pcfo.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3" "$S1" "$S2" "$S3" "$S4" "$MBPID"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
# a joiner is CONNECTING until its links answer: wait for every node ready and seeing the others
st() { # st <n> -> "state peers_up" (nodes 1-3 on 127.0.9, 4-7 on 127.0.10)
	h=127.0.9.$1; [ $1 -ge 4 ] && h=127.0.10.$1
	./perfcli -h $h -p 1750$1 -a "$SECRET" -q -j '{"method":"stats"}' 2>/dev/null \
		| python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
formed() { # formed <n>...: up to 30 s
	i=0
	while [ $i -lt 300 ]; do
		f=0; for n in "$@"; do [ "$(st $n)" = "ready $(($# - 1))" ] || f=1; done
		[ $f = 0 ] && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
formed 1 2 3

# ---- the main leg: run the client, kill the node it names ----------
"$FT" 127.0.9.1 17501 17502 17503 "$SECRET" > "$D/ft.out" 2>&1 &
FTPID=$!
# first cue: the client names a standby's port; kill that node.  The client then proves the idle
# ping finds it while the active serves.
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

# ---- a second, healthy fleet (nodes 4-6) for the legs below: the main leg killed two of its
# three nodes, and round-robin over one usable member puts every client on it
node 4 17504
node 5 17505
node 6 17506
sed -i 's/^mode = store$/mode = shard/' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/239.255.77.76:17176/239.255.77.77:17178/' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/^advertise = 127.0.9./advertise = 127.0.10./' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
sed -i 's/^tcp = 127.0.9./tcp = 127.0.10./' "$D/n4.conf" "$D/n5.conf" "$D/n6.conf"
S1= S2= S3=
start 4 S1; start 5 S2; start 6 S3
formed 4 5 6

# ---- policy leg: independent clients must not all pick one node (round-robin starts at random
# per client)
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
# $SAN, or the link fails against a sanitized libperfd.a (make exports a command-line SAN).
# A compile failure is a failure, not a skip: only an absent compiler skips.
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

# ---- per-key routing removes the forward hop, proved by the daemons' fwd_sent: routed near zero,
# the same load unrouted a lot.  Read through perfcli, the only thing here that speaks Noise.
fleet_fwd() {
	F=0
	for hp in "127.0.10.4 17504" "127.0.10.5 17505" "127.0.10.6 17506"; do
		set -- $hp
		V=$(./perfcli -h "$1" -p "$2" -a "$SECRET" stats 2>/dev/null \
			| tr -d ' \n' | sed -n 's/.*"fwd_sent":\([0-9]*\).*/\1/p')
		# not ${V:-0}: an unanswered node has not said it forwarded nothing.  `exit` would only leave
		# the command substitution, so record it for the caller.
		[ -z "$V" ] && { echo "$1:$2" >> "$D/fwd.unreadable"; continue; }
		F=$((F + V))
	done
	echo "$F"
}
# control first: unrouted load must make the counter climb.  The tool check goes here, not in
# fleet_fwd, where `exit` only leaves the subshell.
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


# ---- the whole fleet restarts under a handle; the seed stays down.  Cues RESTART-ALL, KILL-ALL,
# START-OTHERS; perfd_redial must reach the fleet through a learned member.
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

# ---- a member bounced inside the purge window, a node added, a node expelled; the restart leg
# left node 4 down, so bring it back first
start 4 S1
formed 4 5 6
node 7 17507
sed -i 's/^mode = store$/mode = shard/' "$D/n7.conf"
sed -i 's/239.255.77.76:17176/239.255.77.77:17178/' "$D/n7.conf"
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
if [ "$M_BAD" -gt 0 ]; then
	# a red member leg prints what the handle saw at its first failed write, each node's refused
	# senders and the fleet's own account of the bounce
	grep "^  at the first failed write: " "$D/mb.out"
	for n in 4 5 6; do
		./perfcli -h 127.0.10.$n -p 1750$n -a "$SECRET" stats 2>/dev/null | python3 -c '
import json, sys
want = ("sender_refused", "fwd_sent", "fwd_served", "fwd_fails", "fwd_no_route",
        "pull_sent", "pull_timeouts")
got = {}
def walk(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k in want and not isinstance(v, (dict, list)) and k not in got:
                got[k] = v
            walk(v)
    elif isinstance(o, list):
        for v in o:
            walk(v)
try:
    walk(json.load(sys.stdin))
except Exception as e:
    got["unreadable"] = str(e)[:60]
print("  node '"$n"': " + " ".join("%s=%s" % (k, got[k]) for k in want if k in got) +
      (" unreadable=" + got["unreadable"] if "unreadable" in got else ""))'
	done
	for n in 4 5 6 7; do
		grep -h "cluster: refused a \|cluster: not following" "$D/n$n.log" 2>/dev/null \
			| head -10 | cut -c1-240 | sed "s/^/  n$n refused| /"
		grep -h "cluster:" "$D/n$n.log" 2>/dev/null | tail -30 | cut -c1-240 | sed "s/^/  n$n| /"
	done
fi

# the legs above leave nodes 4-6 up and 7 gone
for v in "$S1" "$S2" "$S3" "$S4"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; done
echo "failovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
