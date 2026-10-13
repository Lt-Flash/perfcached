#!/bin/sh
# colnametest.sh - S343: one collection-name rule - 1-32 characters of
# A-Z a-z 0-9 _ -, not starting with '-' - for whatever ORIGINATES a name.
#
# The config file always held names to it (less the leading '-'); the
# runtime create/rename and a peer's announce took any bytes, so
# create "<img src=x onerror=...>" was accepted, replicated to every member
# and persisted (the operator, 2026-10-07: "introduce collection name checks
# and min/max length so nobody would be able to inject some XSS").
#  1. config: [collection -x] is refused by -C (the old check allowed it);
#  2. create/rename: markup, 33 characters, a .nofall name, a space, a
#     leading '-', a dot - all refused with the rule's own words; a
#     32-character name is accepted, and a rename INTO a bad name refused;
#  3. a peer: node A carries a name from before the rule in its own
#     collections file - KEPT there, said loudly - and announces it; node B
#     refuses it, counts it (cluster.bad_col_names) and never creates it.
# Usage: test/colnametest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccn.XXXXXX)
PIDS=
trap 'for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PC_ENABLE=cn-enable-secret; export PC_ENABLE
RULE="collection names are 1-32 characters"

conf() { # conf <id> <port> [cluster-advertise]
	mkdir -p "$D/st$1"
	{
	printf '[daemon]\nworkers = 2\nlog_level = notice\nstate_dir = %s/st%s\nallow_create = yes\n' "$D" "$1"
	printf '[memory]\narena_mb = 32\n[secrets]\nclient = cn-client-secret\nenable = cn-enable-secret\ncluster = cn-cluster-secret\n'
	printf '[listen]\ntcp = 127.0.0.1:%s\nplaintext = loopback\n' "$2"
	[ -n "${3:-}" ] && printf '[cluster]\nmulticast = 239.255.77.71:17171\nadvertise = %s\n' "$3"
	printf '[collection 0]\nbuckets_log2 = 8\n'
	} > "$D/n$1.conf"
	chmod 640 "$D/n$1.conf"
}
start() { # start <id>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.log"; exit 1
}
j() { # j <port> <method> [json params]  - privileged per link, as coltest
	python3 - "$@" <<'PYEOF'
import json, os, pcnative, socket, sys
port, method = int(sys.argv[1]), sys.argv[2]
params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
s = socket.create_connection(("127.0.0.1", port), timeout=20); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 0, "method": "enable",
    "params": {"secret": os.environ["PC_ENABLE"]}}) + "\n").encode())
f.flush(); f.readline()
r = {"jsonrpc": "2.0", "id": 1, "method": method}
if params is not None: r["params"] = params
f.write((json.dumps(r) + "\n").encode()); f.flush()
print(json.dumps(json.loads(f.readline())))
PYEOF
}
cols() { j "$1" collections | python3 -c "import json,sys; print(' '.join(sorted(c['name'] for c in json.load(sys.stdin)['result']['collections'])))"; }

# 1. the config's own check, now the one rule
conf 9 17790
printf '[collection -lead]\nbuckets_log2 = 8\n' >> "$D/n9.conf"
OUT=$("$BIN" -C -f "$D/n9.conf" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$OUT" | grep -q "$RULE"; then
	ok "config: a name starting with '-' is refused, in the rule's words"
else
	bad "config: [collection -lead] rc $rc: $(echo "$OUT" | tail -1)"
fi

# 2. create and rename on one node
conf 1 17791
start 1
for n in '<img src=x onerror=alert(1)>' 'abcdefghijklmnopqrstuvwxyz0123456' 'a.nofall' 'x y' '-lead' 'a.b'; do
	R=$(j 17791 create "$(python3 -c 'import json,sys; print(json.dumps({"col": sys.argv[1]}))' "$n")")
	case "$R" in
	*"$RULE"*) ok "create '$n' refused with the rule";;
	*) bad "create '$n': $R";;
	esac
done
LONG=abcdefghijklmnopqrstuvwxyz012345
R=$(j 17791 create "{\"col\":\"$LONG\"}")
case "$R" in *'"created": true'*) ok "create of a 32-character name is accepted";; *) bad "32 characters: $R";; esac
R=$(j 17791 rename "{\"col\":\"$LONG\",\"to\":\"<b>x</b>\"}")
case "$R" in *"$RULE"*) ok "rename INTO markup is refused with the rule";; *) bad "rename into markup: $R";; esac
R=$(j 17791 rename "{\"col\":\"$LONG\",\"to\":\"renamed_ok\"}")
case "$R" in *'"renamed": true'*|*'"ok": true'*|*'"result": {'*) ok "rename to a good name is accepted";; *) bad "rename to a good name: $R";; esac
C=$(cols 17791)
case " $C " in *" renamed_ok "*) ok "and the collection set holds only good names ($C)";; *) bad "collections: $C";; esac

# 3. a peer announcing a name from before the rule
conf 2 17792 127.0.71.2
conf 3 17793 127.0.71.3
BADN='x<svg/onload=1>'
printf 'perfcached-collections 1\ncollection %s 8 1 1\n' "$BADN" > "$D/st2/collections"
start 2
grep -q "breaks the collection-name rule" "$D/n2.log" \
	&& ok "node A keeps a name from before the rule out of its own file, and says so" \
	|| bad "node A said nothing about the name it loaded: $(grep -i -m1 collection "$D/n2.log")"
C=$(cols 17792)
case " $C " in *" $BADN "*) ok "and serves it - its data is not dropped";; *) bad "node A collections: $C";; esac
start 3
n=0; i=0
while [ $i -lt 100 ]; do
	n=$(j 17793 stats | python3 -c "import json,sys; print(json.load(sys.stdin)['result']['cluster'].get('bad_col_names', 0))" 2>/dev/null)
	[ "${n:-0}" -ge 1 ] && break
	sleep 0.2; i=$((i+1))
done
[ "${n:-0}" -ge 1 ] && ok "node B refused the announced name and counts it (bad_col_names $n)" \
	|| bad "node B never refused it (bad_col_names ${n:-?})"
C=$(cols 17793)
case " $C " in *"<svg"*) bad "node B created the bad name: $C";; *) ok "node B never created it ($C)";; esac
grep -q "announced by a peer refused" "$D/n3.log" \
	&& ok "node B's log names the refusal, the bytes printed safely ($(grep -m1 -o "collection '[^']*' announced" "$D/n3.log"))" \
	|| bad "node B's log has no refusal line"

echo "colnametest: $pass passed, $fail failed"
[ $fail -eq 0 ]
