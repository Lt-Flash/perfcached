#!/bin/sh
# restarttest.sh - a whole eager fleet restarting AT ONCE keeps its data: two holders is normal in eager
# mode, so a second positive pull answer must not demote a copy (the sweep re-sends only authored keys).
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrestart.XXXXXX)
P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
# fsync = always so the readback is no bet on flush timing; an ack is at the producer
# ring and a full ring drops the record, so the assertions read wal.dropped
fsync = always
# small segments on purpose: the default 8 x 256 MB provisions 2 GB per node, and
# the suite needs durability across a restart, not capacity
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
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
f = pcnative.wrap(s)
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

# "appended dropped late" off /stats, empty when the WAL is off; the premise asserts content, since
# segment_mb preallocates and an empty wal dir measures 16 MB
walst() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    sys.exit(0)
w = d.get("wal")
if not w:
    sys.exit(0)
print("%d %d %d" % (w.get("appended", 0), w.get("dropped", 0),
        w.get("late", 0)))' "$1" 2>/dev/null; }
# "perfcached ready" is the LISTENER; writes are refused until the node STATE is ready, so wait for
# the state on every node
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

# the premise: wait for the pump to drain (appended == NKEYS) before asserting no loss - kill -9
# spares the page cache but takes records still in a producer ring
walsum() {
	DUR=0; APP=0; DROP=0; LATE=0
	for n in 1 2 3; do
		ws=$(walst $((18470 + n)))
		[ -n "$ws" ] || continue
		wa=$(echo "$ws" | cut -d' ' -f1)
		wd=$(echo "$ws" | cut -d' ' -f2)
		wl=$(echo "$ws" | cut -d' ' -f3)
		[ "${wa:-0}" -gt 0 ] && DUR=$((DUR+1))
		APP=$((APP + ${wa:-0})); DROP=$((DROP + ${wd:-0}))
		LATE=$((LATE + ${wl:-0}))
	done
}
w=0
while [ $w -lt 60 ]; do
	walsum
	[ "$APP" -ge "$NKEYS" ] && break
	sleep 0.5; w=$((w+1))
done
echo "    wal: appended=$APP dropped=$DROP late=$LATE across the fleet" \
	"(drained in $(echo "$w" | awk '{printf "%.1f", $1/2}')s)"
[ "$DUR" -ge 1 ] && ok "a node has the keyspace on disk ($DUR of 3 APPENDED wal records)" \
	|| bad "no node appended a wal record - the restart below would prove nothing"
[ "$APP" -ge "$NKEYS" ] \
	&& ok "the wal drained before the kill (appended $APP of $NKEYS)" \
	|| echo "  note: the pump did NOT drain in 30s - $APP of $NKEYS appended," \
		"so $((NKEYS - APP)) record(s) are still in a producer ring and a" \
		"kill -9 will take them; dropped=$DROP"
kill -9 $P1 $P2 $P3 2>/dev/null; P1= P2= P3=
sleep 1
start 1 1 || exit 1
start 2 2 || exit 1
start 3 3 || exit 1
readyall || echo "  (nodes slow to reach ready after the restart)"

# the high-water mark per node separates recovering short (durability) from shedding after recovery
# (the demotion caught here); the wait lets any demotion fire (the bug shed within ~30s)
HA=0; HB=0; HC=0; FA=-1; FB=-1; FC=-1
w=0
while [ $w -lt 20 ]; do
	a=$(ent 18471); b=$(ent 18472); c=$(ent 18473)
	[ $w -eq 0 ] && { FA=$a; FB=$b; FC=$c; }
	[ "${a:-0}" -gt "$HA" ] && HA=$a
	[ "${b:-0}" -gt "$HB" ] && HB=$b
	[ "${c:-0}" -gt "$HC" ] && HC=$c
	echo "    [t=$((w*3))s] $a/$b/$c"
	sleep 3; w=$((w+1))
	[ "$a" = "$NKEYS" ] && [ "$b" = "$NKEYS" ] && [ "$c" = "$NKEYS" ] && [ $w -ge 4 ] && break
done
a=$(ent 18471); b=$(ent 18472); c=$(ent 18473)
[ "${a:-0}" -gt "$HA" ] && HA=$a
[ "${b:-0}" -gt "$HB" ] && HB=$b
[ "${c:-0}" -gt "$HC" ] && HC=$c
echo "  after a simultaneous restart: $a/$b/$c (each owes $NKEYS)"
echo "    first sample $FA/$FB/$FC, high-water $HA/$HB/$HC"

# 1. going DOWN from what a node already served is the demotion bug; recovering short is a
# durability question
[ "${a:-0}" -ge "$HA" ] && [ "${b:-0}" -ge "$HB" ] && [ "${c:-0}" -ge "$HC" ] \
	&& ok "no node shed a record it had already recovered ($HA/$HB/$HC -> $a/$b/$c)" \
	|| bad "a node SHED records after recovering them: high-water $HA/$HB/$HC -> final $a/$b/$c"

# 2. all three hold the same count: a shed copy stays shed, as the sweep re-sends only what a node
# AUTHORED
[ "$a" = "$b" ] && [ "$b" = "$c" ] \
	&& ok "all three converged on the same count ($a)" \
	|| bad "the fleet did not converge: $a/$b/$c - a node holds a different set from its peers"

# 3. zero loss only where the WAL accepted everything; otherwise recovery owes exactly what reached
# the log (APP), and still fails when a logged record does not come back
if [ "${APP:-0}" -ge "$NKEYS" ] && [ "${DROP:-0}" -eq 0 ]; then
	[ "$a" = "$NKEYS" ] && [ "$b" = "$NKEYS" ] && [ "$c" = "$NKEYS" ] \
		&& ok "the whole keyspace was logged and the whole keyspace came back ($NKEYS)" \
		|| bad "all $NKEYS records were in the wal yet only $a/$b/$c came back - a LOGGED record was lost"
else
	echo "  note: the wal held $APP of $NKEYS at the kill (dropped=$DROP);"
	echo "        a staged write is acked before it is appended (src/wal.h), and"
	echo "        kill -9 takes whatever is still in a producer ring"
	[ "$a" -ge "$APP" ] && [ "$b" -ge "$APP" ] && [ "$c" -ge "$APP" ] \
		&& ok "every record that reached the wal came back ($APP of $NKEYS logged, $a/$b/$c held)" \
		|| bad "recovery returned less than the wal held: $APP logged, $a/$b/$c came back"
fi
BR=$(grep -hc "birth race" "$D"/node.*.log 2>/dev/null | paste -sd+ | python3 -c 'import sys; print(sum(int(x) for x in sys.stdin.read().split("+")))' 2>/dev/null || echo 0)
[ "${BR:-0}" = "0" ] \
	&& ok "and no birth race was declared at all" \
	|| bad "$BR birth-race demotions logged - identical records are being treated as conflicts"

echo "restarttest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
