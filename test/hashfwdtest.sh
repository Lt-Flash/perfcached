#!/bin/sh
# hashfwdtest.sh - S319: a forwarded hash READ is rendered on the asking
# node, over the record the owner sends - so its reply is no longer bound
# by the forward ack (58,000 bytes) that a reply TREE had to fit.
#
#   1  a shard fleet of three: a hash whose HGETALL reply is larger than
#      a forward ack reads the same through the owner and through both
#      forwarders, on RESP and on the native door; the small reads, the
#      absent key, WRONGTYPE and the arity errors read the same through
#      a forwarder as on the owner; writes still forward; the ceiling on
#      a command's ARGUMENTS is one number wherever it runs - an HMGET
#      over 58,000 bytes of arguments is refused on the owner too, by
#      name; fwd_reply_too_big stays 0 and nothing is logged
#   2  the gate: beside a member that neither advertises the read plane
#      nor asks on it (a variant of this tree, built as hashtest's part 4
#      does, or the older daemon given as $2) every node asks the old
#      way - a small
#      read through the old member works, the big HGETALL through a
#      forwarder answers "holder rejected", and the OWNER now counts it
#      and logs why; replace that member with this build and the big read
#      works through every node: the rolling restart
# Fail-first: on 0.4.5.2 the big HGETALL through a forwarder is
# "-ERR holder rejected", nothing counts it, nothing logs it, the
# argument ceiling is "command too large to forward" on a forwarder only,
# and an HMGET of 400 absent fields through a forwarder is "bad reply"
# (the RESP sink sized its buffer at twice the tree; a nil is 1 -> 5).
# Usage: test/hashfwdtest.sh [./perfcached] [older-perfcached-for-part-2]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
OLD=${2:-}
NF=1500                                # 16+16-byte fields: record 57,008 B,
                                       # HGETALL tree 63,005 B (> 58,000)
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18371 18372 18373 18381 18382 18383 18361 18362 18363; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "hashfwdtest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pchfwd.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = hf-client-secret
cluster = hf-cluster-secret
[listen]
tcp = 127.0.0.1:1837$1
resp = 127.0.0.1:1838$1
http = 127.0.0.1:1836$1
plaintext = loopback
[cluster]
multicast = 239.255.80.37:18390
advertise = 127.0.40.$1
mode = shard
collections = 0
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
# py <mode> <n> <args...>: one RESP connection to node n
#   cmd  <args...>   one command; an array reply prints as [a,b,...], an
#                    error as -LINE, a nil as nil
#   count <args...>  one command; prints the array's length or the error
#   load <key> <nf>  HSETs nf 16+16-byte fields (f%015d -> v%015d) in
#                    chunks of 250 pairs; prints the sum of the replies
#   hgetall <key> <nf>  HGETALL, checked field by field; prints "ok <n>"
#   hmget <key> <nfields> <namelen>  HMGET with that many names of that
#                    length (existing names when namelen is 16); prints
#                    the array's length or the error
py() { m=$1; n=$2; shift 2; python3 - "$m" "1838$n" "$@" <<'PY'
import socket, sys
mode, port = sys.argv[1], int(sys.argv[2])
a = sys.argv[3:]
s = socket.create_connection(("127.0.0.1", port), timeout=30)
f = s.makefile("rb")
def enc(args):
    return ("*%d\r\n" % len(args) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in args)).encode()
def rd():
    l = f.readline().decode().rstrip("\r\n")
    t, v = l[0], l[1:]
    if t == "*": return [rd() for _ in range(int(v))] if v != "-1" else None
    if t == "$": return None if v == "-1" else f.read(int(v) + 2)[:-2].decode()
    if t == ":": return int(v)
    if t == "+": return v
    return ("err", v)
def show(x):
    if x is None: return "nil"
    if isinstance(x, tuple): return "-" + x[1]
    if isinstance(x, list): return "[" + ",".join(show(y) for y in x) + "]"
    return str(x)
def count(x):
    if isinstance(x, list): return str(len(x))
    return show(x)
fld = lambda i: "f%015d" % i
val = lambda i: "v%015d" % i
if mode == "cmd":
    s.sendall(enc(a)); print(show(rd()))
elif mode == "count":
    s.sendall(enc(a)); print(count(rd()))
elif mode == "load":
    key, nf = a[0], int(a[1])
    tot = 0
    for i in range(0, nf, 250):
        args = ["HSET", key]
        for k in range(i, min(i + 250, nf)):
            args += [fld(k), val(k)]
        s.sendall(enc(args))
        r = rd()
        if not isinstance(r, int):
            print(show(r)); sys.exit(0)
        tot += r
    print(tot)
elif mode == "hgetall":
    key, nf = a[0], int(a[1])
    s.sendall(enc(["HGETALL", key])); r = rd()
    if not isinstance(r, list):
        print(show(r)); sys.exit(0)
    if len(r) != 2 * nf:
        print("bad length %d" % len(r)); sys.exit(0)
    got = dict(zip(r[0::2], r[1::2]))
    bad = sum(1 for k in range(nf) if got.get(fld(k)) != val(k))
    print("ok %d" % len(r) if bad == 0 else "bad %d fields" % bad)
elif mode == "hmget":
    key, nn, ln = a[0], int(a[1]), int(a[2])
    names = [fld(k) if ln == 16 else ("x%0" + str(ln - 1) + "d") % k for k in range(nn)]
    s.sendall(enc(["HMGET", key] + names)); print(count(rd()))
PY
}
# j <n> <method> <params-json>: the native door; prints the result as JSON
j() { python3 - "1837$1" "$2" "$3" <<'PY'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": sys.argv[2], "params": json.loads(sys.argv[3])}) + "\n").encode()); f.flush()
m = json.loads(f.readline())
r = m.get("result", m.get("error"))
print(len(r) if isinstance(r, list) else json.dumps(r, sort_keys=True))
PY
}
# st <n> <counter>: one cluster counter off /stats, wherever it nests;
# "missing" when the daemon does not publish it
st() { curl -s "http://127.0.0.1:1836$1/stats" | python3 -c '
import json, sys
want = sys.argv[1]
def find(x):
    if isinstance(x, dict):
        if want in x: return x[want]
        for v in x.values():
            r = find(v)
            if r is not None: return r
    return None
r = find(json.load(sys.stdin)); print("missing" if r is None else r)' "$2"; }
fleet() {
	i=0; while [ $i -lt 120 ]; do
		good=1
		for x in "$@"; do
			n=$(curl -s "http://127.0.0.1:1836$x/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0))' 2>/dev/null)
			[ "$n" = "$#" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# owned_by <n> <prefix>: prints a key the shard map gives node n - the
# first probe key whose SET through node n was NOT forwarded (fwd_sent
# unchanged); the probe is deleted again
owned_by() {
	k=1; while [ $k -le 60 ]; do
		key="$2$k"
		b=$(st "$1" fwd_sent)
		py cmd "$1" SET "$key" 1 > /dev/null
		a=$(st "$1" fwd_sent)
		py cmd "$1" DEL "$key" > /dev/null
		[ "$a" = "$b" ] && { echo "$key"; return 0; }
		k=$((k+1))
	done
	return 1
}
eq() { # eq <label> <got> <want>
	[ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"
}
others() { case $1 in 1) echo "2 3";; 2) echo "1 3";; 3) echo "1 2";; esac; }

# ---- 1. a shard fleet of three -------------------------------------------
echo "part 1: a shard fleet of three, a hash whose HGETALL is larger than a forward ack"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the shard fleet did not form"; echo "hashfwdtest: $pass passed, $fail failed"; exit 1; }
sleep 2
big=$(owned_by 1 big) || { bad "no key of 60 is owned by node 1"; echo "hashfwdtest: $pass passed, $fail failed"; exit 1; }
set -- $(others 1); f1=$1; f2=$2
echo "  key $big is owned by node 1; nodes $f1 and $f2 forward"
eq "HSET $NF 16+16-byte fields through a forwarder" "$(py load $f1 "$big" $NF)" $NF
eq "HLEN on the owner" "$(py cmd 1 HLEN "$big")" $NF
eq "HGETALL on the owner" "$(py hgetall 1 "$big" $NF)" "ok $((NF * 2))"
eq "HGETALL through forwarder $f1" "$(py hgetall $f1 "$big" $NF)" "ok $((NF * 2))"
eq "HGETALL through forwarder $f2" "$(py hgetall $f2 "$big" $NF)" "ok $((NF * 2))"
eq "native hcmd HGETALL through forwarder $f1" "$(j $f1 hcmd "{\"col\":\"0\",\"key\":\"$big\",\"cmd\":\"HGETALL\"}")" $((NF * 2))
eq "native hcmd HGET through forwarder $f2" "$(j $f2 hcmd "{\"col\":\"0\",\"key\":\"$big\",\"cmd\":\"HGET\",\"args\":[\"f000000000000007\"]}")" '"v000000000000007"'
eq "HGET through a forwarder" "$(py cmd $f1 HGET "$big" f000000000000042)" v000000000000042
eq "HGET of an absent field through a forwarder" "$(py cmd $f2 HGET "$big" nope)" nil
eq "HMGET through a forwarder" "$(py cmd $f1 HMGET "$big" f000000000000001 nope f000000000000003)" "[v000000000000001,nil,v000000000000003]"
eq "HLEN through a forwarder" "$(py cmd $f2 HLEN "$big")" $NF
eq "HEXISTS through a forwarder" "$(py cmd $f1 HEXISTS "$big" f000000000000001)$(py cmd $f1 HEXISTS "$big" nope)" 10
eq "HSTRLEN through a forwarder" "$(py cmd $f2 HSTRLEN "$big" f000000000000001)$(py cmd $f2 HSTRLEN "$big" nope)" 160
eq "HKEYS through a forwarder" "$(py count $f1 HKEYS "$big")" $NF
eq "HVALS through a forwarder" "$(py count $f2 HVALS "$big")" $NF
eq "HSCAN MATCH through a forwarder" "$(py cmd $f1 HSCAN "$big" 0 MATCH 'f00000000000001*')" "[0,[f000000000000010,v000000000000010,f000000000000011,v000000000000011,f000000000000012,v000000000000012,f000000000000013,v000000000000013,f000000000000014,v000000000000014,f000000000000015,v000000000000015,f000000000000016,v000000000000016,f000000000000017,v000000000000017,f000000000000018,v000000000000018,f000000000000019,v000000000000019]]"
eq "HSCAN of everything through a forwarder" "$(py count $f2 HSCAN "$big" 0)" 2
eq "HRANDFIELD 2 WITHVALUES through a forwarder" "$(py count $f1 HRANDFIELD "$big" 2 WITHVALUES)" 4
eq "HMGET of 400 existing names through a forwarder" "$(py hmget $f2 "$big" 400 16)" 400
eq "the arity error through a forwarder" "$(py cmd $f1 HGET "$big")" "-ERR wrong number of arguments for 'hget' command"
eq "the absent key through a forwarder: HGETALL / HGET / HLEN / HMGET" "$(py cmd $f1 HGETALL nokey)$(py cmd $f2 HGET nokey f)$(py cmd $f1 HLEN nokey)$(py cmd $f2 HMGET nokey a b)" "[]nil0[nil,nil]"
py cmd 1 SET "${big}s" plain > /dev/null
eq "WRONGTYPE through every node" "$(py cmd 1 HGET "${big}s" f)|$(py cmd $f1 HGETALL "${big}s")|$(py cmd $f2 HLEN "${big}s")" "-WRONGTYPE Operation against a key holding the wrong kind of value|-WRONGTYPE Operation against a key holding the wrong kind of value|-WRONGTYPE Operation against a key holding the wrong kind of value"
eq "HINCRBY through a forwarder still forwards" "$(py cmd $f1 HINCRBY "$big" n 5)" 5
eq "and the owner and the other forwarder read it" "$(py cmd 1 HGET "$big" n)$(py cmd $f2 HGET "$big" n)" 55
eq "HDEL through a forwarder" "$(py cmd $f2 HDEL "$big" n)" 1
w="-ERR hash command too large: its arguments exceed 58000 bytes"
eq "HMGET with 500 x 120-byte names on the OWNER" "$(py hmget 1 "$big" 500 120)" "$w"
eq "the same through a forwarder: the same words" "$(py hmget $f1 "$big" 500 120)" "$w"
eq "and 400 x 120-byte names fit, on both" "$(py hmget 1 "$big" 400 120)$(py hmget $f2 "$big" 400 120)" 400400
a=""; for n in 1 2 3; do a="$a$(st $n fwd_reply_too_big) "; done
eq "fwd_reply_too_big on every node" "$a" "0 0 0 "
e=$(cat "$D"/n*.log | grep -c 'exceeds the 58000-byte ack ceiling')
eq "no refusal logged" "$e" 0
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
for n in 1 2 3; do stopn $n; done

# ---- 2. the gate ------------------------------------------------------------
if [ -z "$OLD" ]; then
	echo "--- building the variant that does not advertise the read plane"
	mkdir -p "$D/t"
	tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
		(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
	# neither advertises the plane nor asks on it - what 0.4.5.2 does
	sed -i 's/CLMEMB_FEAT_TYPES2 | CLMEMB_FEAT_HREAD/CLMEMB_FEAT_TYPES2/' "$D/t/src/cluster.c"
	sed -i 's/pc_cluster_fleet_has(PC_FEAT_HREAD, NULL)/0/' "$D/t/src/verbs.c"
	grep -q 'CLMEMB_FEAT_HREAD;' "$D/t/src/cluster.c" && { bad "the variant still advertises CLMEMB_FEAT_HREAD"; exit 1; }
	grep -q 'pc_cluster_fleet_has(PC_FEAT_HREAD' "$D/t/src/verbs.c" && { bad "the variant still asks on the read plane"; exit 1; }
	if make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.old.log" 2>&1; then
		OLD="$D/t/perfcached"
	else
		bad "the variant did not build: $(tail -3 "$D/build.old.log" | tr '\n' ' ')"
	fi
fi
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
	echo "part 2: the gate - node 3 does not advertise the read plane"
	for n in 1 2 3; do rm -rf "$D/s$n"; conf $n; done
	start 1 "$BIN" && start 2 "$BIN" && start 3 "$OLD" || exit 1
	fleet 1 2 3 || { bad "the mixed fleet did not form"; echo "hashfwdtest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	bo=$(owned_by 1 gate) || { bad "no key of 60 is owned by node 1"; echo "hashfwdtest: $pass passed, $fail failed"; exit 1; }
	echo "  key $bo is owned by node 1 (this build)"
	eq "2. HSET $NF fields through node 3" "$(py load 3 "$bo" $NF)" $NF
	eq "2. a small read through the old member" "$(py cmd 3 HGET "$bo" f000000000000001)" v000000000000001
	eq "2. a small read through node 2 (asked the old way)" "$(py cmd 2 HMGET "$bo" f000000000000001 f000000000000002)" "[v000000000000001,v000000000000002]"
	eq "2. HGETALL on the owner" "$(py hgetall 1 "$bo" $NF)" "ok $((NF * 2))"
	eq "2. the big HGETALL through node 2: refused, as before" "$(py hgetall 2 "$bo" $NF)" "-ERR holder rejected"
	eq "2. and through the old member" "$(py hgetall 3 "$bo" $NF)" "-ERR holder rejected"
	c=$(st 1 fwd_reply_too_big)
	[ "$c" != missing ] && [ "$c" -ge 2 ] 2>/dev/null && ok "2. the owner counted both refusals: fwd_reply_too_big $c" || bad "2. owner's fwd_reply_too_big: $c"
	e=$(grep -c 'exceeds the 58000-byte ack ceiling' "$D/n1.log")
	[ "$e" -ge 2 ] 2>/dev/null && ok "2. and logged why ($e lines)" || bad "2. owner's log has $e refusal lines"
	echo "  rolling restart: node 3 comes back as this build"
	stopn 3
	start 3 "$BIN" || exit 1
	fleet 1 2 3 || { bad "2. the fleet did not re-form after the restart"; echo "hashfwdtest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	eq "2. after the restart: HGETALL through node 2" "$(py hgetall 2 "$bo" $NF)" "ok $((NF * 2))"
	eq "2. after the restart: HGETALL through node 3" "$(py hgetall 3 "$bo" $NF)" "ok $((NF * 2))"
	eq "2. and the owner" "$(py hgetall 1 "$bo" $NF)" "ok $((NF * 2))"
	e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
	[ "$e" = 0 ] && ok "2. no ERROR/CRIT" || bad "2. $e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
	for n in 1 2 3; do stopn $n; done
fi
echo "hashfwdtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
