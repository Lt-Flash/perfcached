#!/bin/sh
# uploopguardtest.sh - S384 (0.6.1): no ring of read-throughs.  A perfcached
# read-through upstream that is itself a perfcached (INFO server says
# perfcached_version) is refused unless upstream_allow_perfcached = yes;
# allowed, the fetch connection names itself (CLIENT SETNAME perfcached-
# readthrough) and that node never reads through on its behalf - so a key
# crosses at most one perfcached hop, and a ring (A <-> B, a node reading
# its own door) answers a miss at once instead of bouncing until every
# hop's timeout (single-flight is per node: across nodes it would storm).
# Two real nodes, A and B, upstream_timeout_ms = 2000 (a ring that waited
# would take that long):
#  1. default: A's upstream is B - refused, "a perfcached node"; A's read
#     answers an error; the log says why; the alert names it
#  2. allowed: A reads B's own record (one hop); B lists A's fetcher by name
#  3. a ring, both allowed (A -> B, B -> A), a key neither holds: nil in
#     well under the timeout; B fetched nothing onward; both nodes warn
#     chain.0 (read through by another perfcached) and count the reads
#  4. A reading its own door (allowed): nil at once, one fetch, no ERROR
#  5. config: a bad value, the key without an upstream - refused
# Fail-first: 0.6.0.1 refuses upstream_allow_perfcached as an unknown key.
# Usage: test/uploopguardtest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pcuploop.XXXXXX)
AT=19861 AR=19862 AH=19863 BT=19864 BR=19865 BH=19866
PA= PB=
trap 'for p in $PA $PB; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $AT $AR $AH $BT $BR $BH; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "uploopguardtest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60

conf() { # conf <name> <tcp> <resp> <http> [collection 0 lines...]
	n=$1 t=$2 r=$3 h=$4; shift 4
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/$n.state
[memory]
arena_mb = 32
[secrets]
client = loop-client-secret
[listen]
tcp = 127.0.0.1:$t
resp = 127.0.0.1:$r
http = 127.0.0.1:$h
plaintext = loopback
[collection 0]
buckets_log2 = 8
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
start() { # start <name> - sets P
	rm -rf "$D/$1.state"; mkdir -p "$D/$1.state"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & P=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do
		kill -0 $P 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stopn() { [ -n "$1" ] && { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; }; }
resp() { # resp <port> <command words...> - the reply's first line or value; and the time it took (ms) on fd 3
	python3 - "$@" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rb")
a = [x.encode() for x in sys.argv[2:]]
t0 = time.time()
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
l = f.readline()
ms = int((time.time() - t0) * 1000)
if l.startswith(b"$") and not l.startswith(b"$-1"):
    out = f.read(int(l[1:]) + 2)[:-2].decode()
elif l.startswith(b"$-1"):
    out = "(nil)"
else:
    out = l.decode().strip()
print("%s|%d" % (out, ms))
PY
}
up() { # up <http port> <python expr over u (collection 0's upstream block)>
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read())
u=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null
}
alert() { # alert <http port> <key> - "level|text" or none
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read())
print(next(("%s|%s" % (a["level"], a["text"]) for a in s["alerts"]["active"] if a["key"]==sys.argv[2]), "none"))' "$1" "$2" 2>/dev/null
}
waitfor() { w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1; }

UPB="upstream = redis://127.0.0.1:$BR"
UPA="upstream = redis://127.0.0.1:$AR"
COMMON="upstream_prefixes = k:*"
TMO="upstream_timeout_ms = 2000"

# ---- B: a plain node holding k:held ------------------------------------------------
conf b $BT $BR $BH
start b && PB=$P || { bad "B did not start: $(tail -2 "$D/b.log")"; echo "uploopguardtest: $pass passed, $fail failed"; exit 1; }
resp $BR SET k:held v-held > /dev/null

# ---- 1. default: a perfcached upstream is refused -------------------------------------
conf a1 $AT $AR $AH "$UPB" "$COMMON" "$TMO"
if start a1; then
	PA=$P
	r=$(resp $AR GET k:held)
	case "$r" in -*) ok "1. A's read through B answers an error (${r%|*})";; *) bad "1. A read '$r'";; esac
	waitfor 5 '[ "$(up $AH "u[\"upstreams\"][0][\"why\"]")" = "a perfcached: daisy-chaining off" ]'
	[ "$(up $AH 'u["upstreams"][0]["why"]')" = "a perfcached: daisy-chaining off" ] && ok "1. /stats: B is not used - 'a perfcached: daisy-chaining off'" ||
		bad "1. /stats: $(up $AH 'u["upstreams"]')"
	grep -q "upstream 127.0.0.1:$BR is a perfcached node - not used" "$D/a1.log" && ok "1. the log says why" ||
		bad "1. log: $(grep -m2 'upstream 127' "$D/a1.log")"
	waitfor 6 '[ "$(alert $AH upstream.0)" != none ]'
	case "$(alert $AH upstream.0)" in "warning|"*"a perfcached: daisy-chaining off"*) ok "1. the alert names it: $(alert $AH upstream.0 | cut -c1-100)";;
		*) bad "1. alert: $(alert $AH upstream.0)";; esac
	stopn $PA; PA=
else
	bad "1. A did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a1.log")"
fi

# ---- 2. allowed: one hop ---------------------------------------------------------------------
conf a2 $AT $AR $AH "$UPB" "$COMMON" "$TMO" "upstream_allow_perfcached = yes"
if start a2; then
	PA=$P
	r=$(resp $AR GET k:held)
	[ "${r%|*}" = v-held ] && ok "2. allowed: A reads B's record through B (${r#*|} ms)" || bad "2. A read '$r'"
	resp $BR CLIENT LIST | grep -q "name=perfcached-readthrough" && ok "2. B lists A's fetch connection by name" ||
		bad "2. B's clients: $(resp $BR CLIENT LIST | cut -c1-160)"
	stopn $PA; PA=
else
	bad "2. A did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a2.log")"
fi

# ---- 3. a ring: A -> B -> A, both allowed -----------------------------------------------------
stopn $PB; PB=
conf b3 $BT $BR $BH "$UPA" "$COMMON" "$TMO" "upstream_allow_perfcached = yes"
conf a3 $AT $AR $AH "$UPB" "$COMMON" "$TMO" "upstream_allow_perfcached = yes"
if start b3 && PB=$P && start a3; then
	PA=$P
	sleep 1
	r=$(resp $AR GET k:nowhere)
	ms=${r#*|}
	fb=$(up $BH 'u["upstreams"][0]["fetches"]') fa=$(up $AH 'u["upstreams"][0]["fetches"]')
	[ "${r%|*}" = "(nil)" ] && [ "$ms" -lt 1000 ] && [ "$fb" = 0 ] && [ "$fa" = 1 ] &&
		ok "3. the ring: nil in $ms ms (the timeout is 2000), A fetched once, B fetched nothing onward" ||
		bad "3. ring: '$r', A fetches $fa, B fetches $fb"
	r=$(resp $BR GET k:nowhere2)
	fa2=$(up $AH 'u["upstreams"][0]["fetches"]')
	[ "${r%|*}" = "(nil)" ] && [ "${r#*|}" -lt 1000 ] && [ "$fa2" = "$fa" ] &&
		ok "3. and the other way round: nil in ${r#*|} ms, A fetched nothing for B" || bad "3. from B: '$r', A fetches $fa -> $fa2"
	# both ends see the daisy-chain: each node was read through by the
	# other's fetcher on a collection it reads through itself
	waitfor 6 '[ "$(alert $BH chain.0)" != none ] && [ "$(alert $AH chain.0)" != none ]'
	case "$(alert $BH chain.0)/$(alert $AH chain.0)" in "warning|another perfcached reads collection 0"*/"warning|another perfcached reads collection 0"*)
		ok "3. both nodes warn: chain.0 (mailed like any alert)";;
		*) bad "3. chain alerts: B '$(alert $BH chain.0)' A '$(alert $AH chain.0)'";; esac
	[ "$(up $BH 'u["chained_reads"]')" -ge 1 ] 2>/dev/null && ok "3. /stats chained_reads $(up $BH 'u["chained_reads"]') on B" ||
		bad "3. B chained_reads '$(up $BH 'u.get("chained_reads")')'"
	stopn $PA; PA=
else
	bad "3. the ring did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/b3.log" "$D/a3.log")"
fi
stopn $PB; PB=

# ---- 4. a node reading its own door ---------------------------------------------------------
conf a4 $AT $AR $AH "$UPA" "$COMMON" "$TMO" "upstream_allow_perfcached = yes"
if start a4; then
	PA=$P
	r=$(resp $AR GET k:self)
	[ "${r%|*}" = "(nil)" ] && [ "${r#*|}" -lt 1000 ] && [ "$(up $AH 'u["upstreams"][0]["fetches"]')" = 1 ] &&
		ok "4. reading its own door: nil in ${r#*|} ms, one fetch" || bad "4. self: '$r', fetches $(up $AH 'u["upstreams"][0]["fetches"]')"
	stopn $PA; PA=
else
	bad "4. did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a4.log")"
fi

# ---- 5. config -------------------------------------------------------------------------------
conf c1 $AT $AR $AH "$UPB" "$COMMON" "upstream_allow_perfcached = maybe"
"$BIN" -C -f "$D/c1.conf" > "$D/c1.out" 2>&1
[ $? -ne 0 ] && grep -q "upstream_allow_perfcached: no|yes" "$D/c1.out" && ok "5. a bad value is refused" || bad "5. value: $(tail -1 "$D/c1.out")"
conf c2 $AT $AR $AH "upstream_allow_perfcached = yes"
"$BIN" -C -f "$D/c2.conf" > "$D/c2.out" 2>&1
[ $? -ne 0 ] && grep -q "upstream_allow_perfcached without an upstream" "$D/c2.out" && ok "5. the key without an upstream is refused" ||
	bad "5. alone: $(tail -1 "$D/c2.out")"

e=$(cat "$D"/*.log | grep -E ' (ERROR|CRIT)' | grep -vc ' alert: ')
[ "$e" = 0 ] && ok "no ERROR/CRIT besides the alerts raised" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/*.log | grep -v ' alert: ' | head -2)"
echo "uploopguardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
