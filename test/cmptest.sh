#!/bin/sh
# cmptest.sh - S297: compare-and-set and compare-and-delete, in Redis
# 8.4's spelling: SET key value IFEQ cmp / IFNE cmp and DELEX key
# [IFEQ cmp / IFNE cmp]; the native door's setifeq/setifne/delifeq/delifne.
#
#   1  one node: every reply below is what redis 8.10.2 answered for the
#      same command (probed 2026-10-05) - absent/present/equal/different,
#      PX with IFEQ, IFEQ beside NX/XX (syntax error), a hash key
#      (WRONGTYPE; DELEX: "Key should be of string type ..."), DELEX
#      without a condition is DEL, the arity errors, a counter compares
#      as its decimal text; IFDEQ/IFDNE are refused by name (no DIGEST);
#      the token lock - acquire NX, a wrong token neither refreshes nor
#      releases, the right one does - on RESP and the native door
#   2  a shard fleet of three: the same lock through the two non-owners
#      (decided at the owner), WRONGTYPE and the DELEX type error from
#      the owner
#   3  an eager fleet of three, 40 rounds: two clients on two nodes race
#      SET L tokB|tokC IFEQ tokA - exactly one wins every round, and every
#      node then reads the winner's token
#   4  an owner that predates S297 (a variant of this tree without the
#      owner's branch, or $2): a forwarded SET IFEQ / DELEX is "holder
#      rejected", never a quiet store or delete, and the key is untouched
# Fail-first: 0.4.6 answers "unsupported SET option" and "unknown command".
# Usage: test/cmptest.sh [./perfcached] [older-perfcached-for-part-4]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
OLD=${2:-}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 17731 17732 17733 17735 17736 17737 17771 17772 17773; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "cmptest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pccmp.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n> <mode: none|eager|shard>
	mkdir -p "$D/s$1/wal"
	case $2 in
	none) CL= ;;
	*) CL="[cluster]
multicast = 239.255.80.41:17779
advertise = 127.0.41.$1
mode = $2
collections = 0" ;;
	esac
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = cmp-client-secret
cluster = cmp-cluster-secret
[listen]
tcp = 127.0.0.1:1773$1
resp = 127.0.0.1:1777$1
http = 127.0.0.1:1773$(( $1 + 4 ))
plaintext = loopback
$CL
[collection 0]
buckets_log2 = 12
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
C
	chmod 600 "$D/n$1.conf"
}
start() {
	: > "$D/n$1.log"
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
stopn() { [ -f "$D/n$1.pid" ] && { p=$(cat "$D/n$1.pid"); kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$1.pid"; }; }
# r <n> <args...>: one RESP command, the reply flattened to one line
r() { n=$1; shift; python3 - "1777$n" "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
a = sys.argv[2:]
s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode())
f = s.makefile("rb")
def rd():
    l = f.readline().decode().rstrip("\r\n")
    t, v = l[0], l[1:]
    if t == "*": return "[" + ",".join(rd() for _ in range(int(v))) + "]"
    if t == "$": return "nil" if v == "-1" else f.read(int(v) + 2)[:-2].decode()
    return v if t in ":+" else "-" + v
print(rd())
PY
}
# j <n> <method> <params-json>: the native door; the result (or the error) as JSON
j() { python3 - "1773$1" "$2" "$3" <<'PY'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": sys.argv[2], "params": json.loads(sys.argv[3])}) + "\n").encode()); f.flush()
m = json.loads(f.readline())
print(json.dumps(m.get("result", m.get("error")), sort_keys=True))
PY
}
fleet() {
	i=0; while [ $i -lt 120 ]; do
		good=1
		for x in "$@"; do
			n=$(curl -s "http://127.0.0.1:1773$(( x + 4 ))/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0))' 2>/dev/null)
			[ "$n" = "$#" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
fwd_sent() { curl -s "http://127.0.0.1:1773$(( $1 + 4 ))/stats" | python3 -c '
import json, sys
def find(x, k):
    if isinstance(x, dict):
        if k in x: return x[k]
        for v in x.values():
            r = find(v, k)
            if r is not None: return r
print(find(json.load(sys.stdin), "fwd_sent"))'; }
# owned_by <n> <prefix>: a key the map gives node n (a SET through n not forwarded)
owned_by() {
	k=1; while [ $k -le 60 ]; do
		key="$2$k"; b=$(fwd_sent "$1")
		r "$1" SET "$key" probe > /dev/null
		a=$(fwd_sent "$1"); r "$1" DEL "$key" > /dev/null
		[ "$a" = "$b" ] && { echo "$key"; return 0; }
		k=$((k+1))
	done
	return 1
}
eq() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"; }
W="-WRONGTYPE Operation against a key holding the wrong kind of value"
T="-ERR Key should be of string type if conditions are specified"

# ---- 1. one node -------------------------------------------------------
echo "part 1: one node - Redis 8.10.2's replies"
conf 1 none; start 1 "$BIN" || exit 1
eq "SET IFEQ on an absent key" "$(r 1 SET a v1 IFEQ x)" nil
eq "  and nothing stored" "$(r 1 EXISTS a)" 0
eq "SET IFNE on an absent key" "$(r 1 SET a v1 IFNE x)" OK
eq "  stored" "$(r 1 GET a)" v1
eq "SET IFNE on an equal value" "$(r 1 SET a v2 IFNE v1)" nil
eq "  kept" "$(r 1 GET a)" v1
eq "SET IFEQ on an equal value, PX 5000" "$(r 1 SET a v2 IFEQ v1 PX 5000)" OK
t=$(r 1 TTL a)
[ "$t" -ge 4 ] 2>/dev/null && [ "$t" -le 5 ] && ok "  the PX took ($t s)" || bad "  TTL after IFEQ PX 5000: $t"
eq "IFEQ beside NX" "$(r 1 SET a v5 IFEQ v2 NX)" "-ERR syntax error"
eq "IFEQ beside XX" "$(r 1 SET a v5 IFEQ v2 XX)" "-ERR syntax error"
eq "IFEQ beside IFNE" "$(r 1 SET a v5 IFEQ v2 IFNE v2)" "-ERR syntax error"
eq "  none of them stored" "$(r 1 GET a)" v2
r 1 HSET h f v > /dev/null
eq "SET IFEQ on a hash" "$(r 1 SET h x IFEQ v)" "$W"
eq "SET IFNE on a hash" "$(r 1 SET h x IFNE v)" "$W"
eq "DELEX IFEQ on a hash" "$(r 1 DELEX h IFEQ v)" "$T"
eq "DELEX without a condition is DEL, any type" "$(r 1 DELEX h)$(r 1 EXISTS h)" 10
eq "DELEX IFEQ / IFNE on an absent key" "$(r 1 DELEX nokey IFEQ v)$(r 1 DELEX nokey IFNE v)" 00
r 1 SET b 1 > /dev/null
eq "DELEX IFNE on an equal value" "$(r 1 DELEX b IFNE 1)" 0
eq "DELEX IFNE on a different value" "$(r 1 DELEX b IFNE 2)" 1
eq "DELEX of the deleted key" "$(r 1 DELEX b)" 0
eq "DELEX arity" "$(r 1 DELEX)" "-ERR wrong number of arguments for 'delex' command"
eq "DELEX arity (a condition without its value)" "$(r 1 DELEX b IFEQ)" "-ERR wrong number of arguments for 'delex' command"
r 1 SET c 10 > /dev/null; r 1 INCR c > /dev/null
eq "a counter compares as its decimal text" "$(r 1 SET c 12 IFEQ 11)$(r 1 GET c)" OK12
case "$(r 1 SET c 13 IFDEQ abc)$(r 1 DELEX c IFDNE abc)" in
*"IFDEQ/IFDNE are not supported"*"IFDEQ/IFDNE are not supported"*) ok "IFDEQ / IFDNE refused by name (no DIGEST)";;
*) bad "IFDEQ / IFDNE: $(r 1 SET c 13 IFDEQ abc) / $(r 1 DELEX c IFDNE abc)";; esac
echo "  the token lock, RESP"
eq "acquire" "$(r 1 SET L tokA NX EX 10)" OK
eq "a second client cannot acquire" "$(r 1 SET L tokB NX EX 10)" nil
eq "a wrong token does not refresh" "$(r 1 SET L tokB IFEQ tokX EX 30)" nil
eq "the holder refreshes" "$(r 1 SET L tokA IFEQ tokA EX 30)" OK
t=$(r 1 TTL L); [ "$t" -ge 28 ] 2>/dev/null && ok "  ttl now $t" || bad "  ttl after the refresh: $t"
eq "a wrong token does not release" "$(r 1 DELEX L IFEQ tokX)$(r 1 GET L)" 0tokA
eq "the holder releases" "$(r 1 DELEX L IFEQ tokA)$(r 1 EXISTS L)" 10
echo "  the token lock, the native door"
eq "setnx" "$(j 1 setnx '{"col":"0","key":"NL","value":"tokA","ttl":10}')" '{"stored": true}'
eq "setifeq with a wrong token" "$(j 1 setifeq '{"col":"0","key":"NL","value":"tokB","cmp":"tokX","ttl":30}')" '{"stored": false}'
eq "setifeq with the holder's token" "$(j 1 setifeq '{"col":"0","key":"NL","value":"tokA","cmp":"tokA","ttl":30}')" '{"stored": true}'
eq "delifeq with a wrong token" "$(j 1 delifeq '{"col":"0","key":"NL","cmp":"tokX"}')" '{"deleted": false}'
eq "delifeq with the holder's token" "$(j 1 delifeq '{"col":"0","key":"NL","cmp":"tokA"}')" '{"deleted": true}'
eq "setifne on an absent key" "$(j 1 setifne '{"col":"0","key":"NN","value":"v","cmp":"x"}')" '{"stored": true}'
eq "delifne on an equal value" "$(j 1 delifne '{"col":"0","key":"NN","cmp":"v"}')" '{"deleted": false}'
r 1 HSET h2 f v > /dev/null
case "$(j 1 setifeq '{"col":"0","key":"h2","value":"x","cmp":"v"}')" in *WRONGTYPE*) ok "setifeq on a hash: WRONGTYPE";; *) bad "setifeq on a hash: $(j 1 setifeq '{"col":"0","key":"h2","value":"x","cmp":"v"}')";; esac
case "$(j 1 delifeq '{"col":"0","key":"h2","cmp":"v"}')" in *"string type"*) ok "delifeq on a hash: the string-type error";; *) bad "delifeq on a hash: $(j 1 delifeq '{"col":"0","key":"h2","cmp":"v"}')";; esac
stopn 1

# ---- 2. a shard fleet ----------------------------------------------------
echo "part 2: a shard fleet of three - decided at the owner"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n shard; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the shard fleet did not form"; echo "cmptest: $pass passed, $fail failed"; exit 1; }
sleep 2
L=$(owned_by 1 lock) || { bad "no key of 60 is owned by node 1"; echo "cmptest: $pass passed, $fail failed"; exit 1; }
echo "  key $L is owned by node 1; nodes 2 and 3 forward"
eq "acquire through node 2" "$(r 2 SET "$L" tokA NX EX 30)" OK
eq "a wrong token through node 3: no refresh" "$(r 3 SET "$L" tokB IFEQ tokX)" nil
eq "the holder's token through node 3: refreshed" "$(r 3 SET "$L" tokA IFEQ tokA EX 60)" OK
eq "  the owner holds it" "$(r 1 GET "$L")" tokA
eq "a wrong token through node 2: no release" "$(r 2 DELEX "$L" IFEQ tokX)" 0
eq "the holder releases through node 3" "$(r 3 DELEX "$L" IFEQ tokA)" 1
eq "  gone on the owner" "$(r 1 EXISTS "$L")" 0
eq "native setifne through node 2" "$(j 2 setifne "{\"col\":\"0\",\"key\":\"$L\",\"value\":\"v\",\"cmp\":\"x\"}")" '{"stored": true}'
eq "native delifeq through node 3" "$(j 3 delifeq "{\"col\":\"0\",\"key\":\"$L\",\"cmp\":\"v\"}")" '{"deleted": true}'
r 1 HSET "$L" f v > /dev/null
eq "SET IFEQ on a hash, through node 2" "$(r 2 SET "$L" x IFEQ v)" "$W"
eq "DELEX IFEQ on a hash, through node 3" "$(r 3 DELEX "$L" IFEQ v)" "$T"
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
for n in 1 2 3; do stopn $n; done

# ---- 3. an eager fleet race ----------------------------------------------
echo "part 3: an eager fleet of three - 40 races, one winner each"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n eager; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the eager fleet did not form"; echo "cmptest: $pass passed, $fail failed"; exit 1; }
sleep 2
python3 - > "$D/race.out" 2>&1 <<'PY'
import socket, threading, time
def conn(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=10); return s, s.makefile("rb")
def cmd(c, *a):
    s, f = c
    s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    l = f.readline().decode().rstrip("\r\n")
    if l.startswith("$"):
        n = int(l[1:]); return None if n < 0 else f.read(n + 2)[:-2].decode()
    return l
c1, c2, c3 = conn(17771), conn(17772), conn(17773)
bad = 0
for rnd in range(40):
    key = "race%d" % rnd
    # acquired as the idiom does - SET NX, decided on the key's deciding
    # node - so the node that decides the race holds tokA already (a
    # plain SET lands on node 1 and reaches the decider by push)
    if cmd(c1, "SET", key, "tokA", "NX") != "+OK":
        bad += 1; print("ROUND %d: the NX acquire failed" % rnd); continue
    res = {}
    bar = threading.Barrier(2)
    def go(name, c, tok):
        bar.wait(); res[name] = cmd(c, "SET", key, tok, "IFEQ", "tokA")
    t2 = threading.Thread(target=go, args=("B", c2, "tokB"))
    t3 = threading.Thread(target=go, args=("C", c3, "tokC"))
    t2.start(); t3.start(); t2.join(); t3.join()
    wins = [n for n, v in res.items() if v == "+OK"]
    time.sleep(0.15)                      # the winner's push lands
    vals = {cmd(c, "GET", key) for c in (c1, c2, c3)}
    want = {"tok" + wins[0]} if len(wins) == 1 else None
    if len(wins) != 1 or vals != want:
        bad += 1
        print("ROUND %d: replies %s, values %s" % (rnd, res, sorted(map(str, vals))))
print("BAD", bad)
PY
b=$(sed -n 's/^BAD //p' "$D/race.out")
[ "$b" = 0 ] && ok "40 rounds: exactly one SET IFEQ won each, and all three nodes read its token" \
	|| bad "$b of 40 rounds wrong: $(grep ROUND "$D/race.out" | head -3 | tr '\n' ' ') $(tail -2 "$D/race.out" | tr '\n' ' ')"
for n in 1 2 3; do stopn $n; done

# ---- 4. an owner that predates S297 ----------------------------------------
if [ -z "$OLD" ]; then
	echo "--- building the variant whose owner has no S297 branch"
	mkdir -p "$D/t"
	tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
		(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
	sed -i 's/} else if (op >= 6 \&\& op <= 9) {/} else if (0) {/' "$D/t/src/cluster.c"
	grep -q '} else if (0) {' "$D/t/src/cluster.c" || { bad "the variant still has the owner's branch"; exit 1; }
	if make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.old.log" 2>&1; then
		OLD="$D/t/perfcached"
	else
		bad "the variant did not build: $(tail -3 "$D/build.old.log" | tr '\n' ' ')"
	fi
fi
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
	echo "part 4: node 1 (the owner) predates S297"
	for n in 1 2 3; do rm -rf "$D/s$n"; conf $n shard; done
	start 1 "$OLD" && start 2 "$BIN" && start 3 "$BIN" || exit 1
	fleet 1 2 3 || { bad "the mixed fleet did not form"; echo "cmptest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	L=$(owned_by 1 old) || { bad "no key of 60 is owned by node 1"; echo "cmptest: $pass passed, $fail failed"; exit 1; }
	r 1 SET "$L" tokA > /dev/null
	eq "4. SET IFEQ through node 2: refused, not stored" "$(r 2 SET "$L" tokB IFEQ tokA)" "-ERR holder rejected"
	eq "4. DELEX IFEQ through node 3: refused, not deleted" "$(r 3 DELEX "$L" IFEQ tokA)" "-ERR holder rejected"
	eq "4. the key is untouched on the owner" "$(r 1 GET "$L")" tokA
	for n in 1 2 3; do stopn $n; done
fi
echo "cmptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
