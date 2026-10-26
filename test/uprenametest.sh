#!/bin/sh
# uprenametest.sh - S448: a read-through collection is not renamed live.
#
# An upstream binds a collection, and its .nofall companion, by the name in
# the config.  `rename` moved the store's name only: the collection stopped
# reading and writing through, silently, and the binding stayed on a name
# no table answers.  One node, collection 0 reading through from
# test/fakeupstream.py, and a plain collection beside it; privileged native
# calls (allow_create, enable):
#   1  rename of the read-through collection is refused, by name - and it
#      still reads through afterwards
#   2  rename of its .nofall companion is refused
#   3  rename of another collection INTO the bound name is refused, by name
#   4  a plain collection still renames, and answers under the new name
# FAIL-FIRST: before S448, 1 answers renamed and collection 0 is gone - a
# read of it then says "no such collection".
# Usage: test/uprenametest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=19041 TCP=19042 HTTP=19043
for p in $UP $TCP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "uprenametest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcupren.XXXXXX)
PID= FPID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; [ -n "$FPID" ] && kill -9 $FPID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$D/up" "$D/state/wal"
cat > "$D/up/data.json" <<'J'
{"tok:a": {"type": "string", "value": "alpha"},
 "tok:b": {"type": "string", "value": "beta"}}
J
echo '{"password": "ur-pass-1", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = ur-client-secret
enable = ur-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_password = ur-pass-1
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
[collection plain]
buckets_log2 = 10
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the node did not start: $(grep -m1 -E 'CRIT|ERR' "$D/n.log" | cut -c1-160)"
	echo "uprenametest: $pass passed, $fail failed"
	exit 1
fi
# nc <method> [k=v ...]: a privileged native call; prints the result or the error
nc() { python3 - "$TCP" "$@" <<'P' 2>&1
import json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10))
pcnative.call(f, "enable", secret="ur-enable-secret")
kw = dict(a.split("=", 1) for a in sys.argv[3:])
r = pcnative.call(f, sys.argv[2], **kw)
print(json.dumps(r.get("result", r.get("error"))))
P
}
cols() { python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
print(" ".join(sorted(c["name"] for c in r["collections"])))' 2>&1; }
got() { nc get col="$1" key="$2" | python3 -c 'import json,sys
r=json.loads(sys.stdin.read()); print(r.get("value") if isinstance(r, dict) and r.get("found") else r)' 2>&1; }

[ "$(got 0 tok:a)" = alpha ] && ok "collection 0 reads through (tok:a = alpha); collections: $(cols)" \
	|| bad "collection 0 did not read through before anything: $(got 0 tok:a)"

# 1. the read-through collection
R=$(nc rename col=0 to=ren0)
case "$R" in *"not renamed live"*) ok "1. rename of the read-through collection is refused: $R" ;;
	*) bad "1. rename of the read-through collection: $R" ;; esac
V=$(got 0 tok:b)
[ "$V" = beta ] && ok "1. and it still reads through under its name (tok:b = beta)" \
	|| bad "1. after the rename attempt collection 0 answers: $V (collections: $(cols))"

# 2. its companion
R=$(nc rename col=0.nofall to=nf2)
case "$R" in *"not renamed live"*) ok "2. rename of the .nofall companion is refused" ;;
	*) bad "2. rename of the .nofall companion: $R" ;; esac

# 3. into the bound name
R=$(nc rename col=plain to=0)
case "$R" in *"would bind this collection"*) ok "3. rename INTO the read-through's name is refused, by name" ;;
	*) bad "3. rename into the bound name: $R" ;; esac

# 4. a plain collection still renames
nc set col=plain key=k value=pv > /dev/null
R=$(nc rename col=plain to=plain2)
case "$R" in *'"renamed": true'*) ;; *) R="NOT RENAMED: $R" ;; esac
V=$(got plain2 k)
[ "$V" = pv ] && case "$R" in NOT*) false ;; *) true ;; esac \
	&& ok "4. a plain collection renames (plain -> plain2) and answers under the new name" \
	|| bad "4. plain rename: $R; plain2/k = $V"
C=$(cols)
case " $C " in *" 0 "*" 0.nofall "*) ok "the read-through pair is still there: $C" ;;
	*) bad "collections after: $C" ;; esac

grep -q "CRIT" "$D/n.log" && bad "the node logged CRIT: $(grep -m2 CRIT "$D/n.log" | tr '\n' ' ' | cut -c1-200)"
echo "uprenametest: $pass passed, $fail failed"
[ $fail -eq 0 ]
