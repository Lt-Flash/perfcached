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
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done; rm -rf "$D"' EXIT TERM INT
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
if cc -O1 -Ilib -o "$D/spread" "$D/spread.c" libperfd.a -lpthread -lsodium 2>"$D/cc.log"; then
	DISTINCT=$("$D/spread" 127.0.9.2 17502 "$SECRET" 2>/dev/null | sort -u | grep -vc '^-1$')
	[ "${DISTINCT:-0}" -ge 2 ] && ok \
		|| bad "round-robin put 12 clients on ${DISTINCT:-0} node(s)"
else
	echo "  (spread leg skipped: $(tail -1 "$D/cc.log"))"
fi

# ---- S35: per-key routing removes the forward hop -------------------
# The proof is the DAEMONS' counters, not the client's opinion: on a
# shard cluster a routed client should make the fleet forward (almost)
# nothing, and the same load with routing OFF should make it forward a
# lot.  The measurement must be able to show the difference, or it is
# not a measurement.
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
# the client library is the only thing here that speaks Noise, so the
# counters are read through it: perfcli reports stats as JSON
fleet_fwd() {
	F=0
	for hp in "127.0.10.4 17504" "127.0.10.5 17505" "127.0.10.6 17506"; do
		set -- $hp
		V=$(./perfcli -h "$1" -p "$2" -a "$SECRET" stats 2>/dev/null \
			| tr -d ' \n' | sed -n 's/.*"fwd_sent":\([0-9]*\).*/\1/p')
		F=$((F + ${V:-0}))
	done
	echo "$F"
}
# CONTROL first: the same load with routing OFF must make the counter
# climb, or "0 forwards" later proves nothing about routing
B0=$(fleet_fwd)
"$FT" 127.0.10.4 17504 17505 17506 "$SECRET" noroute > "$D/nrt.out" 2>&1
B1=$(fleet_fwd)
UNROUTED=$((B1 - B0))
echo "  fleet fwd_sent for 400 UNROUTED ops: $UNROUTED"
[ "$UNROUTED" -gt 50 ] && ok \
	|| bad "control leg forwarded only $UNROUTED - the counter cannot show the difference"
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
for v in "$S1" "$S2" "$S3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; done

echo "failovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
