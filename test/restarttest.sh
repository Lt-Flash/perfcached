#!/bin/sh
# restarttest.sh — a whole eager fleet restarting AT ONCE keeps its data.
#
# THE BUG THIS EXISTS FOR, measured on the 245-247 fleet 2026-09-14 while
# upgrading 0.3.5 -> 0.3.7-rc4.  A second positive answer to a pull used
# to mean "birth race" on its own, and the loser's copy was demoted.  In
# EAGER mode two holders is the NORMAL state - every key is on every node
# by design - so when all three nodes restarted together, each recovered
# the same keyspace from its own RDB, every reconcile probe drew two
# positives, and two of the three shed their copies: 1988 records down to
# ~425 each.  It did NOT heal, because the eager sweep re-sends only what
# a node AUTHORED.
#
# Nothing cheaper than a fleet finds this.  It needs durable state on
# every node, a simultaneous restart, and a keyspace big enough that the
# loss is visible - the fleet had been restarted many times before at 3
# records, where 3 -> 1 looks like nothing.
#
# Usage: test/restarttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrestart.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
NKEYS=400
MC=239.255.77.49
MP=17149

node() {
	mkdir -p "$D/state.$1" "$D/wal.$1"
	cat > "$D/node.$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/state.$1
[memory]
arena_mb = 64
[secrets]
client = restart-client-secret
cluster = restart-cluster-secret
[listen]
tcp = 127.0.0.1:$((17470 + $1))
http = 127.0.0.1:$((18470 + $1))
http_allow = 127.0.0.0/8
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.10.$1
pull_timeout_ms = 300
mode = eager
collections = b
[collection b]
buckets_log2 = 12
pull = 1
[wal]
dir = $D/wal.$1
# fsync = ALWAYS, because this suite kills with -9 and then asserts that
# nothing was lost.  `everysec` does not promise that and must not be
# asked to: under ASan on a loaded CI runner the author's WAL tail was
# short by 64 of 400 records, every node converged to 336, and the suite
# reported it as a cluster fault.  Only ONE node has durable state here -
# an eager replica is stored off the WAL, since the author persists it
# and a restarted replica resyncs - so a short tail on the author shows
# up identically on all three, which is exactly what a birth race would
# look like.  The suite is about restart SEMANTICS; it must not also be
# a bet on flush timing.
fsync = always
# small segments on purpose: the default 8 x 256 MB provisions 2 GB PER
# NODE, and three of those fills a test box.  The suite needs durability
# across a restart, not capacity.
segments = 2
segment_mb = 16
EOF
}
start() { # start <n> <var>
	"$BIN" -f "$D/node.$1.conf" > "$D/node.$1.log" 2>&1 &
	eval "P$2=\$!"
	w=0
	while [ $w -lt 200 ]; do
		grep -q "perfcached ready" "$D/node.$1.log" 2>/dev/null && return 0
		sleep 0.1; w=$((w+1))
	done
	echo "  node $1 did not start:"; tail -4 "$D/node.$1.log"; return 1
}
ent() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    print(-1); sys.exit(0)
print(sum(c.get("entries",0) for c in d.get("collections",[])))' "$1" 2>/dev/null || echo -1; }
cli() { timeout 20 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
f = s.makefile("rwb")
n = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"]=1; r["jsonrpc"]="2.0"
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    resp = f.readline()
    n += 1
    if b"error" in resp or not resp:
        print("resp %d: %s" % (n, resp[:150])); break
print("wrote", n)' "$1"; }
allready() {
	w=0
	while [ $w -lt 200 ]; do
		a=$(ent 18471); b=$(ent 18472); c=$(ent 18473)
		[ "$a" = "$1" ] && [ "$b" = "$1" ] && [ "$c" = "$1" ] && return 0
		sleep 0.3; w=$((w+1))
	done
	return 1
}

st() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    print(json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4)).get("state",""))
except Exception:
    print("")' "$1" 2>/dev/null; }
# "perfcached ready" in the log is the LISTENER; the cluster node state
# reaches ready later, and a write before then is refused with "node is
# not READY (recovering)".  Wait for the STATE on every node - the same
# trap clitest and spreadtest both hit.
readyall() {
	w=0
	while [ $w -lt 300 ]; do
		[ "$(st 18471)" = "ready" ] && [ "$(st 18472)" = "ready" ] \
			&& [ "$(st 18473)" = "ready" ] && return 0
		sleep 0.3; w=$((w+1))
	done
	return 1
}

node 1; node 2; node 3
start 1 1 || exit 1
start 2 2 || exit 1
start 3 3 || exit 1
readyall || { echo "  nodes never reached state=ready: $(st 18471)/$(st 18472)/$(st 18473)"; exit 1; }

i=0
while [ $i -lt $NKEYS ]; do
	printf '{"method":"set","params":{"col":"b","key":"rk%03d","value":"v%03d","ttl":3600}}\n' $i $i
	i=$((i+1))
done | cli 17471 2>&1 | tail -2 | sed 's/^/    write: /' 
allready $NKEYS && ok "all three hold the $NKEYS keys before the restart" \
	|| { bad "fleet never converged: $(ent 18471)/$(ent 18472)/$(ent 18473)"; \
	     echo "restarttest: $pass passed, $fail failed"; exit 1; }

# THE PREMISE, CHECKED: at least one node must actually hold the
# keyspace durably, or the restart proves nothing about recovery - it
# would just be three empty nodes agreeing.
sleep 2
DUR=0
for n in 1 2 3; do
	sz=$(du -sk "$D/wal.$n" 2>/dev/null | cut -f1)
	[ "${sz:-0}" -gt 16 ] && DUR=$((DUR+1))
done
[ "$DUR" -ge 1 ] && ok "at least one node has the keyspace on disk ($DUR of 3 carry a WAL)" \
	|| bad "no node persisted anything - the restart below would prove nothing"
kill -9 $P1 $P2 $P3 2>/dev/null; P1= P2= P3=
sleep 1
start 1 1 || exit 1
start 2 2 || exit 1
start 3 3 || exit 1
readyall || echo "  (nodes slow to reach ready after the restart)"

# give reconcile and any demotion the time it would have taken to fire:
# the failure was not slow, the nodes shed within ~30s and then sat flat
w=0
while [ $w -lt 20 ]; do
	a=$(ent 18471); b=$(ent 18472); c=$(ent 18473)
	echo "    [t=$((w*3))s] $a/$b/$c"
	sleep 3; w=$((w+1))
	[ "$a" = "$NKEYS" ] && [ "$b" = "$NKEYS" ] && [ "$c" = "$NKEYS" ] && [ $w -ge 4 ] && break
done
a=$(ent 18471); b=$(ent 18472); c=$(ent 18473)
echo "  after a simultaneous restart: $a/$b/$c (each owes $NKEYS)"
[ "$a" = "$NKEYS" ] && [ "$b" = "$NKEYS" ] && [ "$c" = "$NKEYS" ] \
	&& ok "every node kept its whole keyspace - no node was demoted for holding the same record as its peers" \
	|| bad "a node shed records: $a/$b/$c of $NKEYS - two holders at the same version is being treated as a birth race"
BR=$(grep -hc "birth race" "$D"/node.*.log 2>/dev/null | paste -sd+ | python3 -c 'import sys; print(sum(int(x) for x in sys.stdin.read().split("+")))' 2>/dev/null || echo 0)
[ "${BR:-0}" = "0" ] \
	&& ok "and no birth race was declared at all" \
	|| bad "$BR birth-race demotions logged - identical records are being treated as conflicts"

echo "restarttest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
