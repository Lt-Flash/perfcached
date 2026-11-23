#!/bin/sh
# probefulltest.sh - in proxy mode a write whose holder is unknown probes the fleet first, whatever its key's
# length; one whose probe cannot park (the table full) is refused, never placed unprobed.
# Usage: test/probefulltest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcpfl.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && { kill -CONT $p; kill -9 $p; }; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19831 19832; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "probefulltest: port $p already bound" >&2; exit 1; }
done
for i in 1 2; do
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = pfl-client-secret
cluster = pfl-cluster-secret
[listen]
tcp = 127.0.218.$i:1983$i
plaintext = loopback
[cluster]
multicast = 239.255.77.218:17418
advertise = 127.0.218.$i
pull_timeout_ms = 3000
max_pending = 64
[collection px]
buckets_log2 = 10
mode = proxy
C
	chmod 600 "$D/n$i.conf"
	: > "$D/n$i.log"
	"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	PIDS="$PIDS $!"
	eval "P$i=$!"
done
for i in 1 2; do
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log" | tr '\n' ' ')"; exit 1; }
done
python3 "$T/waitready.py" 127.0.218.1:19831 127.0.218.2:19832 || bad "a node is not ready"
peers() {
	python3 - <<'PY' 2>&1
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.218.1", 19831), 10))
print((pcnative.call(f, "stats").get("result") or {}).get("cluster", {}).get("peers_up"))
PY
}
k=0; while [ $k -lt 75 ] && [ "$(peers)" != 1 ]; do sleep 0.2; k=$((k+1)); done
[ "$(peers)" = 1 ] || { bad "node 1 does not see node 2: peers_up $(peers)"; exit 1; }

# a key node 2 holds, written through node 1 (which holds no locator entry for it): the probe finds node 2
# and the write goes there - for a key of 300 bytes as for a short one
nc() { # nc <i> <method> <json params>: one native call at node i - its result as JSON, or ERR <message>
	python3 - "$@" <<'PY' 2>&1
import json, socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.218.%d" % i, 19830 + i), 10))
r = pcnative.call(f, sys.argv[2], **json.loads(sys.argv[3]))
e = r.get("error")
print("ERR " + str(e.get("message") if isinstance(e, dict) else e) if e else json.dumps(r.get("result"), sort_keys=True))
PY
}
ents() { nc "$1" stats '{"col": "'$2'"}' | python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])' 2>&1; }
LONG=$(python3 -c 'print("L" * 300)')
for c in "px short" "px long"; do
	set -- $c
	col=$1; [ "$2" = long ] && k="$LONG" || k=sk
	nc 2 set '{"col": "'$col'", "key": "'$k'", "value": "a"}' > /dev/null
	e0=$(ents 1 "$col")
	r=$(nc 1 set '{"col": "'$col'", "key": "'$k'", "value": "b"}')
	g=$(nc 2 get '{"col": "'$col'", "key": "'$k'"}')
	e1=$(ents 1 "$col")
	case "$g" in *'"value": "b"'*) [ "$e1" = "$e0" ] && ok "${#k}-byte key: a set through node 1 reached its holder ($r)" \
			|| bad "${#k}-byte key: the holder took it but node 1 kept a copy too ($e0 -> $e1 records)";;
		*) bad "${#k}-byte key: a set through node 1 did not reach its holder - node 2 reads $g, node 1 holds $e0 -> $e1 records (set: $r)";;
	esac
	r=$(nc 1 del '{"col": "'$col'", "key": "'$k'"}')
	g=$(nc 2 get '{"col": "'$col'", "key": "'$k'"}')
	case "$r|$g" in *'"deleted": true'*'"found": false'*) ok "${#k}-byte key: a del through node 1 deleted it at the holder";;
		*) bad "${#k}-byte key: del through node 1: $r, node 2 then reads $g";;
	esac
done

# node 2 stopped: still live to node 1 for 10 s, so every new key's probe parks and none is
# answered; 300 new keys through node 1 at once fill the 64-slot table many times over
kill -STOP "$P2"
R=$(python3 - <<'PY' 2>&1
import json, socket, pcnative
s = socket.create_connection(("127.0.218.1", 19831), 30); f = pcnative.wrap(s)
for i in range(300):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": "set",
        "params": {"col": "px", "key": "pf%d" % i, "value": "v"}}) + "\n").encode())
f.flush()
stored = full = other = 0
for _ in range(300):
    r = json.loads(f.readline())
    if r.get("result", {}).get("stored") is True: stored += 1
    elif "table full" in json.dumps(r.get("error", "")): full += 1
    else: other += 1
print("stored=%d full=%d other=%d" % (stored, full, other))
PY
)
kill -CONT "$P2"
case "$R" in
	"stored=0 full="*) N=${R#*full=}; N=${N%% *}
		[ "$N" -ge 200 ] && ok "no new key placed while its probe could not park; $N refused as the table full ($R)" \
			|| bad "too few refused as the table full: $R";;
	*) bad "with the table full, new keys were placed here unprobed: $R";;
esac
grep -q "parked-request table FULL" "$D/n1.log" && ok "the table did fill (logged)" || bad "the table never filled - the case did not run"

echo "probefulltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
