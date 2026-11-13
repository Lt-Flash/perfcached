#!/bin/sh
# stalebackfilltest.sh - a backfill flag a non-sender never used is dropped once its target is
# established, so it cannot fire when the lowest id leaves.  Usage: test/stalebackfilltest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsb.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <n>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = sb-client-secret
cluster = sb-cluster-secret
[listen]
tcp = 127.0.0.1:1711$1
plaintext = loopback
[cluster]
multicast = 239.255.77.53:17153
advertise = 127.0.1.5$1
mode = eager
collections = s
[collection s]
buckets_log2 = 12
CONF
}
start() { # start <n>  - the log is truncated: a restart must wait for ITS
	# "perfcached ready", not find the previous process's line
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		eval "kill -0 \$P$1" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
CLI="$D/cli.py"
cat > "$CLI" <<'PYEOF'
import json, pcnative, socket, sys
port = int(sys.argv[1]); op = sys.argv[2]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
except Exception:
    print("DOWN"); sys.exit(0)
f = pcnative.wrap(s); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
if op == "node":
    print(call("stats")["result"]["cluster"]["node"])
elif op == "repl_out":
    print(call("stats")["result"]["cluster"]["repl_out"])
elif op == "state":
    r = call("stats")["result"]; print(r.get("state"), r["cluster"].get("peers_up"))
elif op == "entries":
    print(call("stats")["result"]["collections"][0]["entries"])
elif op == "view":
    want = int(sys.argv[3])
    for m in call("members")["result"]["members"]:
        if m.get("node") == want:
            print("%s %s" % (m.get("entries", "?"), m.get("start", "?"))); break
    else:
        print("absent")
elif op == "fill":
    v = "x" * 10000
    for i in range(int(sys.argv[3])):
        call("set", col="s", key="sk%04d" % i, value=v, ttl=900)
    print("filled")
PYEOF
q() { python3 "$CLI" "$@" 2>/dev/null; }   # q <port> node|repl_out|state|entries|fill N
await_entries() { # await_entries <port> <n> <secs>
	i=0
	while [ $i -lt "$3" ]; do
		[ "$(q "$1" entries)" = "$2" ] && return 0
		sleep 1; i=$((i+1))
	done
	return 1
}

conf 1; conf 2; conf 3
start 1 || { echo "node 1 did not start"; tail -3 "$D/n1.log"; exit 1; }
start 2 || { echo "node 2 did not start"; tail -3 "$D/n2.log"; exit 1; }
start 3 || { echo "node 3 did not start"; tail -3 "$D/n3.log"; exit 1; }
# the cold window runs from each node's ready, and a joiner is CONNECTING until its links answer
i=0
while [ $i -lt 300 ] && [ "$(q 17111 state) $(q 17112 state) $(q 17113 state)" != "ready 2 ready 2 ready 2" ]; do
	sleep 0.1; i=$((i+1))
done
# for BACKFILL_SENDER_HOLDOFF_MS after ready every node is cold and skipped by the sender election,
# so two senders would be correct then: wait it out, this suite is about an established non-sender
echo "  waiting out the cold window (35s) so L is the only sender"
sleep 35

# ---- roles from the ids ------------------------------------------------
I1=$(q 17111 node); I2=$(q 17112 node); I3=$(q 17113 node)
case "$I1$I2$I3" in *DOWN*|"") echo "could not read ids ($I1/$I2/$I3)"; exit 1;; esac
L=1; X=1; Y=1
for n in 2 3; do eval "v=\$I$n"; eval "lv=\$I$L"; [ "$v" -lt "$lv" ] && L=$n; done
for n in 2 3; do eval "v=\$I$n"; eval "xv=\$I$X"; [ "$v" -gt "$xv" ] && X=$n; done
for n in 1 2 3; do [ "$n" != "$L" ] && [ "$n" != "$X" ] && Y=$n; done
eval "IL=\$I$L"; eval "IX=\$I$X"; eval "IY=\$I$Y"
echo "  roles: L=node$L(id $IL, sender)  X=node$X(id $IX, restarts)  Y=node$Y(id $IY, stale holder)"
[ "$IL" -lt "$IY" ] && [ "$IY" -lt "$IX" ] && ok "three distinct ids, roles readable" \
	|| { bad "ids not strictly ordered ($IL/$IY/$IX) - cannot assign roles"; echo "stalebackfilltest: $pass passed, $fail failed"; exit 1; }
PL="P$L"; PX="P$X"

# ---- 1. records on L; X restarts cold; L backfills it -------------------
q "1711$L" fill 300 >/dev/null
await_entries "1711$X" 300 30 && await_entries "1711$Y" 300 30 \
	&& ok "300 records on every node" || bad "the eager push did not converge (X=$(q 1711$X entries) Y=$(q 1711$Y entries))"

# ---- 1b. Y's view of L must not flap: twenty samples over three seconds all show L's own
# heartbeat.  A view lags by up to one beat, so let the next heartbeat land first.
sleep 1.5
bad_views=""; i=0
while [ $i -lt 20 ]; do
	V=$(q "1711$Y" view "$IL")
	case "$V" in "300 established") ;; *) bad_views="$bad_views [$V]";; esac
	sleep 0.15; i=$((i+1))
done
[ -z "$bad_views" ] && ok "S103: Y's view of L held steady over 20 samples (300 established)" \
	|| bad "S103: Y's view of L FLAPPED:$bad_views"
eval "kill -9 \$$PX" 2>/dev/null; eval "$PX="
sleep 8                              # past the purge window
start "$X" || bad "X did not restart"
# it came back empty, so it joins RECOVERING and pulls: the state is the property, a count would
# race the pull.  The join is the cluster thread's, a moment after ready: wait for the line.
i=0; while [ $i -lt 75 ] && ! grep -q "holding nothing - pulling a bootstrap" "$D/n$X.log"; do sleep 0.2; i=$((i+1)); done
grep -q "holding nothing - pulling a bootstrap" "$D/n$X.log" \
	&& ok "X came back empty and set out to pull (after $((i / 5))s)" || bad "X did not join as an empty puller: $(grep 'joined as' "$D/n$X.log" | tail -1)"
await_entries "1711$X" 300 90 && ok "X was refilled ($(q 1711$X entries))" \
	|| bad "X was never refilled ($(q 1711$X entries) of 300)"
IX2=$(q "1711$X" node)
[ "$IX2" = "$IX" ] && ok "X kept its id across the restart ($IX)" \
	|| { IX=$IX2; echo "  note: X came back as $IX2"; [ "$IY" -lt "$IX" ] && ok "Y is still below X" || bad "Y ($IY) is no longer below X ($IX): the ordering the test needs is gone"; }
i=0
while [ $i -lt 40 ]; do
	grep -q "bootstrapped from node" "$D/n$X.log" && break
	sleep 1; i=$((i+1))
done
BL=$(grep "bootstrapped from node" "$D/n$X.log" | tail -1)
echo "$BL" | grep -q "bootstrapped from node $IL " && ok "X pulled its bootstrap from L, the lowest ready holder (after ${i}s)" \
	|| bad "X did not pull from L ($IL): $(echo "$BL" | sed 's/.*cluster: //' | cut -c1-100)"
grep -q "backfilled node $IX after its restart" "$D/n$L.log" && bad "L walked a push backfill for a node that pulled" \
	|| ok "L did not walk a push backfill (X pulled)"
grep -q "backfilled node $IX after its restart" "$D/n$Y.log" && bad "Y ALSO walked a backfill - it was not the sender" \
	|| ok "Y did not walk it (as it should not - it is not the lowest)"

# ---- 2. X established: Y and L drop the flags they never used, each at its own tick (L can trail
# Y on a loaded runner), so each is polled for
i=0
while [ $i -lt 60 ]; do
	grep -q "node $IX is established - a backfill armed for it here" "$D/n$Y.log" && break
	sleep 1; i=$((i+1))
done
grep -q "node $IX is established - a backfill armed for it here" "$D/n$Y.log" \
	&& ok "Y dropped its stale flag once X reported established (after ${i}s)" \
	|| bad "Y never dropped the stale flag for $IX (S102 line absent after ${i}s)"
j=0
while [ $j -lt 30 ]; do
	grep -q "node $IX is established - a backfill armed for it here" "$D/n$L.log" && break
	sleep 1; j=$((j+1))
done
grep -q "node $IX is established - a backfill armed for it here" "$D/n$L.log" \
	&& ok "L dropped its flag too (it never walked; ${j}s after Y)" \
	|| bad "L never dropped the flag it armed for $IX (${j}s after Y did)"

# ---- 3. L dies: Y is the lowest live id; it must NOT push at X ----------
RY0=$(q "1711$Y" repl_out)
eval "kill -9 \$$PL" 2>/dev/null; eval "$PL="
i=0; grew=""
while [ $i -lt 40 ]; do
	RYn=$(q "1711$Y" repl_out)
	[ "$RYn" != "$RY0" ] && { grew=$RYn; break; }
	sleep 2; i=$((i+2))
done
[ -z "$grew" ] && ok "Y pushed nothing at X in ${i}s after L died (repl_out $RY0)" \
	|| bad "STALE FLAG FIRED: Y pushed at X after L died (repl_out $RY0 -> $grew in ${i}s)"
grep -q "backfilled node $IX after its restart" "$D/n$Y.log" \
	&& bad "Y logged a FALSE completion for $IX" \
	|| ok "no false completion on Y"
[ "$(q "1711$X" entries)" = "300" ] && ok "X still holds its 300" || bad "X lost records ($(q 1711$X entries))"

echo "stalebackfilltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
