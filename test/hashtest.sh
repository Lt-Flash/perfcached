#!/bin/sh
# hashtest.sh - S313: Redis hashes, as one typed record per key.
#
#   1  one node: every H command's reply and Redis's errors (arity, not an
#      integer / float, overflow, syntax), TYPE hash and WRONGTYPE both
#      ways, the last HDEL deletes the key, HSET keeps the key's TTL, a
#      restart keeps the hash typed, and the native hcmd door
#   2  an eager fleet of three: HSETs of DIFFERENT fields of one hash
#      through all three nodes all land (one node decides each key, so
#      whole-value replication cannot drop one), every node reads the
#      same hash, and an HDEL through another node empties it everywhere
#   3  a shard fleet of three: reads and writes through non-owners are
#      forwarded and answered right - on RESP and the native door
#   4  the 0.4.4 gate: beside a member without the feature byte (a variant
#      of this tree built by the test, as ratelimittest does) HSET is
#      refused naming it
# Fail-first: before S313 every H command is "unknown command".
# Usage: test/hashtest.sh [./perfcached] [older-perfcached-for-part-4]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
OLD=${2:-}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18601 18602 18603 18611 18612 18613 18591 18592 18593; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "hashtest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pchash.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n> <mode: none|eager|shard>
	mkdir -p "$D/s$1/wal"
	case $2 in
	none) CL= ;;
	*) CL="[cluster]
multicast = 239.255.80.35:18600
advertise = 127.0.39.$1
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
client = hs-client-secret
cluster = hs-cluster-secret
[listen]
tcp = 127.0.0.1:1860$1
resp = 127.0.0.1:1861$1
http = 127.0.0.1:1859$1
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
r() { n=$1; shift; python3 - "1861$n" "$@" <<'PY'
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
j() { python3 - "1860$1" "$2" "$3" <<'PY'
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
			n=$(curl -s "http://127.0.0.1:1859$x/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0))' 2>/dev/null)
			[ "$n" = "$#" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
eq() { # eq <label> <got> <want>
	[ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"
}

# ---- 1. one node -----------------------------------------------------------
echo "part 1: one node"
conf 1 none; start 1 "$BIN" || exit 1
eq "HSET two new fields" "$(r 1 HSET h f1 v1 f2 v2)" 2
eq "HSET one new, one updated" "$(r 1 HSET h f2 V2 f3 v3)" 1
eq "HGET" "$(r 1 HGET h f2)" V2
eq "HGET absent field" "$(r 1 HGET h nope)" nil
eq "HGET absent key" "$(r 1 HGET nokey f)" nil
eq "HMGET" "$(r 1 HMGET h f1 nope f3)" "[v1,nil,v3]"
eq "HLEN" "$(r 1 HLEN h)" 3
eq "HLEN absent key" "$(r 1 HLEN nokey)" 0
eq "HEXISTS" "$(r 1 HEXISTS h f1)$(r 1 HEXISTS h nope)" 10
eq "HSTRLEN" "$(r 1 HSTRLEN h f2)$(r 1 HSTRLEN h nope)" 20
eq "HKEYS (insertion order, an update keeps its place)" "$(r 1 HKEYS h)" "[f1,f2,f3]"
eq "HVALS" "$(r 1 HVALS h)" "[v1,V2,v3]"
eq "HGETALL" "$(r 1 HGETALL h)" "[f1,v1,f2,V2,f3,v3]"
eq "HGETALL absent key" "$(r 1 HGETALL nokey)" "[]"
eq "HSETNX present / absent" "$(r 1 HSETNX h f1 x)$(r 1 HSETNX h f4 v4)" 01
eq "HMSET" "$(r 1 HMSET h f5 v5)" OK
eq "TYPE" "$(r 1 TYPE h)" hash
eq "GET on a hash" "$(r 1 GET h)" "-WRONGTYPE Operation against a key holding the wrong kind of value"
r 1 SET s plain > /dev/null
eq "HSET on a string" "$(r 1 HSET s f v)" "-WRONGTYPE Operation against a key holding the wrong kind of value"
eq "HSET odd arity" "$(r 1 HSET h f1)" "-ERR wrong number of arguments for 'hset' command"
eq "HGET arity" "$(r 1 HGET h)" "-ERR wrong number of arguments for 'hget' command"
eq "HINCRBY absent field" "$(r 1 HINCRBY h n 5)" 5
eq "HINCRBY" "$(r 1 HINCRBY h n -7)" -2
eq "HINCRBY on a non-integer" "$(r 1 HINCRBY h f1 1)" "-ERR hash value is not an integer"
eq "HINCRBY by a non-integer" "$(r 1 HINCRBY h n x)" "-ERR value is not an integer or out of range"
r 1 HSET h big 9223372036854775806 > /dev/null
eq "HINCRBY overflow" "$(r 1 HINCRBY h big 2)" "-ERR increment or decrement would overflow"
eq "HINCRBYFLOAT" "$(r 1 HINCRBYFLOAT h fl 10.5)" 10.5
eq "HINCRBYFLOAT again" "$(r 1 HINCRBYFLOAT h fl 0.1)" 10.6
r 1 HSET h e3 5.0e3 > /dev/null
eq "HINCRBYFLOAT on 5.0e3" "$(r 1 HINCRBYFLOAT h e3 200)" 5200
eq "HINCRBYFLOAT on a non-float" "$(r 1 HINCRBYFLOAT h f1 1)" "-ERR hash value is not a float"
eq "HINCRBYFLOAT by a non-float" "$(r 1 HINCRBYFLOAT h fl x)" "-ERR value is not a valid float"
rf=$(r 1 HRANDFIELD h)
case "$rf" in f1|f2|f3|f4|f5|n|big|fl|e3) ok "HRANDFIELD: $rf";; *) bad "HRANDFIELD: $rf";; esac
n=$(r 1 HRANDFIELD h 4 | tr -d '[]' | tr ',' '\n' | sort -u | wc -l)
eq "HRANDFIELD 4: four distinct" "$n" 4
n=$(r 1 HRANDFIELD h 100 | tr -d '[]' | tr ',' '\n' | wc -l)
eq "HRANDFIELD 100 on 9 fields: all 9" "$n" 9
n=$(r 1 HRANDFIELD h -20 | tr -d '[]' | tr ',' '\n' | wc -l)
eq "HRANDFIELD -20: twenty, repeats allowed" "$n" 20
n=$(r 1 HRANDFIELD h 2 WITHVALUES | tr -d '[]' | tr ',' '\n' | wc -l)
eq "HRANDFIELD 2 WITHVALUES: two pairs" "$n" 4
eq "HRANDFIELD bad option" "$(r 1 HRANDFIELD h 2 WITHX)" "-ERR syntax error"
eq "HSCAN MATCH f*" "$(r 1 HSCAN h 0 MATCH 'f[12]')" "[0,[f1,v1,f2,V2]]"
eq "HSCAN NOVALUES" "$(r 1 HSCAN h 0 MATCH 'f[12]' NOVALUES)" "[0,[f1,f2]]"
eq "HSCAN bad cursor" "$(r 1 HSCAN h x)" "-ERR invalid cursor"
r 1 HSET t a 1 > /dev/null; r 1 EXPIRE t 100 > /dev/null; r 1 HSET t b 2 > /dev/null
t=$(r 1 TTL t)
[ "$t" -ge 95 ] 2>/dev/null && [ "$t" -le 100 ] && ok "HSET keeps the key's TTL ($t)" || bad "TTL after HSET: $t"
eq "HDEL two of two" "$(r 1 HDEL t a b nope)" 2
eq "the last HDEL deleted the key" "$(r 1 EXISTS t)" 0
jr=$(j 1 hcmd '{"col":"0","key":"h","cmd":"HMGET","args":["f1","nope"]}')
eq "native hcmd" "$jr" '["v1", null]'
jr=$(j 1 hcmd '{"col":"0","key":"s","cmd":"HGET","args":["f"]}')
eq "native hcmd WRONGTYPE" "$jr" '{"error": "WRONGTYPE Operation against a key holding the wrong kind of value"}'
j 1 sync '{}' > /dev/null
stopn 1; start 1 "$BIN" || exit 1
eq "a restart keeps the hash typed" "$(r 1 TYPE h)$(r 1 HGET h f2)" hashV2
stopn 1

# ---- 2. an eager fleet ------------------------------------------------------
echo "part 2: an eager fleet of three"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n eager; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the eager fleet did not form"; echo "hashtest: $pass passed, $fail failed"; exit 1; }
sleep 2
for i in 1 2 3 4 5 6 7 8 9; do n=$(( (i - 1) % 3 + 1 )); r $n HSET eh "f$i" "v$i" > /dev/null; done
sleep 1
a="$(r 1 HLEN eh) $(r 2 HLEN eh) $(r 3 HLEN eh)"
eq "nine fields through nodes 1,2,3 in turn: all nine on every node" "$a" "9 9 9"
a="$(r 1 HGETALL eh)"
[ "$a" = "$(r 2 HGETALL eh)" ] && [ "$a" = "$(r 3 HGETALL eh)" ] && ok "every node reads the same hash" || bad "the nodes differ: $a / $(r 2 HGETALL eh)"
eq "TYPE on every node" "$(r 1 TYPE eh)$(r 2 TYPE eh)$(r 3 TYPE eh)" hashhashhash
r 3 HDEL eh f1 f2 f3 f4 f5 f6 f7 f8 f9 > /dev/null
sleep 1
eq "HDEL of every field through node 3: gone everywhere" "$(r 1 EXISTS eh)$(r 2 EXISTS eh)$(r 3 EXISTS eh)" 000
for n in 1 2 3; do stopn $n; done

# ---- 3. a shard fleet -------------------------------------------------------
echo "part 3: a shard fleet of three"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n shard; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the shard fleet did not form"; echo "hashtest: $pass passed, $fail failed"; exit 1; }
sleep 2
for k in sa sb sc sd se; do r 1 HSET $k x 1 y 2 > /dev/null; done
a=""; for k in sa sb sc sd se; do a="$a$(r 2 HGET $k y)$(r 3 HLEN $k)"; done
eq "five hashes written through node 1, read through nodes 2 and 3" "$a" 2222222222
eq "HINCRBY through node 3" "$(r 3 HINCRBY sc x 41)" 42
eq "native hcmd through node 2" "$(j 2 hcmd '{"col":"0","key":"sc","cmd":"HGETALL"}')" '["x", "42", "y", "2"]'
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
for n in 1 2 3; do stopn $n; done

# ---- 4. the gate ------------------------------------------------------------
if [ -z "$OLD" ]; then
	echo "--- building the no-features variant (the frame 0.4.3 sends)"
	mkdir -p "$D/t"
	tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
		(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
	# the frame 0.4.3 sends ends reason, epoch: S314's features byte and
	# 0.5.7.1's sole_witness (2) behind it both go - left in, a stock reader
	# takes sole_witness' high byte for the features byte, and a witness id
	# over 255 reads as a node that has the feature (the 0.5.7.1 red)
	sed -i '/pc_w8(&c, (unsigned)in->features);/d; /pc_w16(&c, (uint16_t)in->sole_witness);/d;
		s/buf\[len - 5\]/buf[len - 2]/; s/if (len < 88 + 15)/if (len < 88 + 12)/' \
		"$D/t/src/clmemb.c"
	if grep -qE 'in->(features|sole_witness)\);' "$D/t/src/clmemb.c" ||
	   ! grep -q 'buf\[len - 2\]' "$D/t/src/clmemb.c" || ! grep -q 'len < 88 + 12' "$D/t/src/clmemb.c"; then
		bad "the clmemb.c sed missed - the variant would not be the 0.4.3 frame"
		echo "hashtest: $pass passed, $fail failed"; exit 1
	fi
	if make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.old.log" 2>&1; then
		OLD="$D/t/perfcached"
	else
		bad "the no-features variant did not build: $(tail -3 "$D/build.old.log" | tr '\n' ' ')"
	fi
fi
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
	echo "part 4: the gate"
	for n in 1 2 3; do rm -rf "$D/s$n"; conf $n eager; done
	start 1 "$BIN" && start 2 "$BIN" && start 3 "$OLD" || exit 1
	fleet 1 2 3 || { bad "the mixed fleet did not form"; echo "hashtest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	g=$(r 1 HSET gk f v)
	case "$g" in *"hash commands need every node in the fleet on 0.4.4 or later - node "*) ok "4. with an older member: $g";;
		*) bad "4. with an older member HSET answered: $g";; esac
	for n in 1 2 3; do stopn $n; done
fi
echo "hashtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
