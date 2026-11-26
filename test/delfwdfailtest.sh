#!/bin/sh
# delfwdfailtest.sh - in shard mode a delete whose forward to the key's owner cannot start is refused, as
# a set's is, and never answered "not there" while the owner keeps the key.
# Usage: test/delfwdfailtest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pcdff.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "delfwdfailtest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19821 19822 19823 19824; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "delfwdfailtest: port $p already bound" >&2; exit 1; }
done
: > "$D/cut1"
for i in 1 2; do
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = dff-client-secret
cluster = dff-cluster-secret
[listen]
tcp = 127.0.217.$i:1982$i
resp = 127.0.217.$i:1982$((i + 2))
resp_collections = c
plaintext = loopback
[cluster]
multicast = 239.255.77.217:17417
advertise = 127.0.217.$i
pull_timeout_ms = 300
[collection c]
buckets_log2 = 10
mode = shard
C
	chmod 600 "$D/n$i.conf"
	: > "$D/n$i.log"
	if [ $i = 1 ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut1" PC_NETCUT_LOG="$D/cutlog1" \
			"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	else
		: > "$D/n$i.log"
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
	fi
	PIDS="$PIDS $!"
done
for i in 1 2; do
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$i.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$i.log" || { bad "node $i did not start: $(tail -2 "$D/n$i.log" | tr '\n' ' ')"; exit 1; }
done
python3 "$T/waitready.py" 127.0.217.1:19821 127.0.217.2:19822 || bad "a node is not ready"
# nc <i> <method> <json params>: one native call at node i - its result as JSON, or ERR <message>
nc() {
	python3 - "$@" <<'PY' 2>&1
import json, socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.217.%d" % i, 19820 + i), 10))
r = pcnative.call(f, sys.argv[2], **json.loads(sys.argv[3]))
e = r.get("error")
if e:
    print("ERR " + str(e.get("message") if isinstance(e, dict) else e))
else:
    print(json.dumps(r.get("result"), sort_keys=True))
PY
}
rsp() { # rsp <i> <args...>: one RESP command at node i, its reply line
	python3 - "$@" <<'PY' 2>&1
import socket, sys
s = socket.create_connection(("127.0.217.%s" % sys.argv[1], 19822 + int(sys.argv[1])), 10)
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
print(s.makefile("rb").readline().decode("latin-1").strip())
PY
}
ents() { nc "$1" stats '{"col": "c"}' | python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])' 2>&1; }
peers() { nc 1 stats '{}' | python3 -c 'import json,sys; print(json.load(sys.stdin)["cluster"].get("peers_up"))' 2>&1; }
k=0; while [ $k -lt 75 ] && [ "$(peers)" != 1 ]; do sleep 0.2; k=$((k+1)); done
[ "$(peers)" = 1 ] || { bad "node 1 does not see node 2: peers_up $(peers)"; exit 1; }

# a key node 2 owns: a set through node 1 lands there
K=
for n in $(seq 1 40); do
	e0=$(ents 2)
	r=$(nc 1 set '{"col": "c", "key": "fk:'$n'", "value": "v"}')
	e1=$(ents 2)
	echo "$r" | grep -q '"stored": true' && [ "$e1" -gt "$e0" ] 2>/dev/null && { K=fk:$n; break; }
done
[ -n "$K" ] && ok "$K set through node 1 is stored at its owner, node 2" || { bad "no key set through node 1 landed on node 2"; exit 1; }

# node 1's datagrams to node 2 now fail to send: no forward can start (node 2 stays live for 10 s)
echo "cut 127.0.217.2 eperm" > "$D/cut1"
sleep 0.3
S=$(nc 1 set '{"col": "c", "key": "'$K'", "value": "w"}')
case "$S" in "ERR "*) ok "set $K through node 1, the forward unable to start: refused ($S)";;
	*) bad "set $K through node 1, the forward unable to start: $S (want a refusal)";; esac
R=$(nc 1 del '{"col": "c", "key": "'$K'"}')
case "$R" in "ERR "*) ok "del $K through node 1, the forward unable to start: refused as the set is ($R)";;
	*) bad "del $K through node 1, the forward unable to start: $R (want the set's refusal, not deleted false)";; esac
R=$(rsp 1 DEL "$K")
case "$R" in -*) ok "RESP DEL $K through node 1: $R";;
	*) bad "RESP DEL $K through node 1, the forward unable to start: $R (want an error, not :0)";; esac
# the shim writes its counts at its next send 50 ms on (a beat comes within a second)
k=0; while [ $k -lt 30 ]; do O=$(grep -o 'out [0-9]*' "$D/cutlog1" 2>/dev/null); [ "${O#out }" -gt 0 ] 2>/dev/null && break; sleep 0.1; k=$((k+1)); done
[ "${O#out }" -gt 0 ] 2>/dev/null && ok "the cut fired ($O datagrams)" || bad "the cut never fired: $(cat "$D/cutlog1" 2>/dev/null)"

: > "$D/cut1"
sleep 0.5
G=$(nc 2 get '{"col": "c", "key": "'$K'"}')
echo "$G" | grep -q '"value": "v"' && ok "$K is still on its owner with its value: no refused write happened" || bad "$K on node 2 after the cut: $G"
k=0; R=; while [ $k -lt 20 ]; do R=$(nc 1 del '{"col": "c", "key": "'$K'"}'); echo "$R" | grep -q '"deleted": true' && break; sleep 0.2; k=$((k+1)); done
echo "$R" | grep -q '"deleted": true' && ok "the cut lifted: del $K through node 1 deletes it at the owner" || bad "after the cut: del $K = $R"

echo "delfwdfailtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
