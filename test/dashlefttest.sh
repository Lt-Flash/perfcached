#!/bin/sh
# dashlefttest.sh - a member the dashboard learned leaves the fleet: once no member lists it live and
# its own dial fails five passes running it shows "left" and is not dialled; listed again, it is back.
set -u
BIN=${1:-./perfcached}
DASH=${2:-./perfcached-dashboard}
[ -x "$DASH" ] || { echo "dashlefttest: SKIPPED - $DASH not built (libmicrohttpd-dev absent?)"; exit 0; }
D=$(mktemp -d /var/tmp/pcdleft.XXXXXX)
TCP1=17631 TCP2=17632 HP=17697
PA= PB= PD= PL=
trap 'for p in $PA $PB $PD $PL; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $TCP1 $TCP2 $HP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "dashlefttest: port $p already bound" >&2; exit 1; }
done

node() { # node <n> <tcp>
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = info
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = left-client-secret
cluster = left-cluster-secret
[listen]
tcp = 127.0.0.5$1:$2
[cluster]
multicast = 239.255.77.63:17663
advertise = 127.0.0.5$1
[collection c]
buckets_log2 = 10
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <var>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval "P$2=$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
node 1 $TCP1
node 2 $TCP2
start 1 A
start 2 B

KEY=$(python3 -c 'import os; print(os.urandom(32).hex())')
cat > "$D/dash.conf" <<C
[dashboard]
listen = 127.0.0.1:$HP
seed = 127.0.0.51:$TCP1
secret = left-client-secret
session_key = $KEY
cache_ms = 200
refresh_ms = 300
C
chmod 600 "$D/dash.conf"
"$DASH" -f "$D/dash.conf" > "$D/dash.out" 2> "$D/dash.log" & PD=$!
i=0
while [ $i -lt 50 ] && ! grep -q "perfcached-dashboard ready" "$D/dash.out"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached-dashboard ready" "$D/dash.out" || { echo "the dashboard did not start"; cat "$D/dash.log"; exit 1; }

# state <addr:port>: that member's state on the dashboard's /fleet
state() { python3 - "http://127.0.0.1:$HP/fleet" "$1" <<'PY' 2>/dev/null
import json, sys, urllib.request
d = json.load(urllib.request.urlopen(sys.argv[1], timeout=10))
for m in d.get("members", []):
    if "%s:%d" % (m["addr"], m["port"]) == sys.argv[2]:
        print(m["state"]); break
else:
    print("absent")
PY
}
waitstate() { # waitstate <addr:port> <state> <tenths>
	i=0
	while [ $i -lt "$3" ]; do
		[ "$(state "$1")" = "$2" ] && return 0
		sleep 0.5; i=$((i+5))
	done
	return 1
}
M2=127.0.0.52:$TCP2
waitstate $M2 connected 300 && ok "node 2 learned from node 1's member list and connected" \
	|| { bad "node 2 never connected: $(state $M2)"; echo "dashlefttest: $pass passed, $fail failed"; exit 1; }

kill -9 $PB; PB=
waitstate $M2 left 900 && ok "node 2 gone from the fleet: shown left" || bad "node 2 after it left the fleet: '$(state $M2)'"

# a listener on its address counts what still dials it
cat > "$D/count.py" <<'PY'
import socket, sys
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.52", int(sys.argv[1])))
ls.listen(16)
n = 0
while True:
    c, _ = ls.accept()
    n += 1
    open(sys.argv[2], "w").write("%d\n" % n)
    c.close()
PY
echo 0 > "$D/dials"
python3 "$D/count.py" $TCP2 "$D/dials" > /dev/null 2>&1 & PL=$!
sleep 0.5
i=0; while [ $i -lt 10 ]; do state $M2 > /dev/null; sleep 0.5; i=$((i+1)); done
[ "$(cat "$D/dials")" = 0 ] && ok "a member shown left is not dialled (5 s of refreshes, 0 connections)" \
	|| bad "node 2, left, was dialled $(cat "$D/dials") times in 5 s"
kill -9 $PL; PL=

start 2 B
waitstate $M2 connected 600 && ok "node 2 back in the fleet: listed again, dialled and connected" \
	|| bad "node 2 after it came back: '$(state $M2)'"

echo "dashlefttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
