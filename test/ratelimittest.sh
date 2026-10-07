#!/bin/sh
# ratelimittest.sh - S314: RL.HIT, an atomic sliding-window rate limiter.
#
# One command for what a Redis client does as MULTI { ZADD k now now;
# ZREMRANGEBYSCORE k -inf now-window; ZCOUNT; EXPIRE } (DESIGN 12aa / 12ic):
# record the hit, drop what fell out of the window, answer [count, allowed].
#   1  one node: the count and allowed at the limit's edge; TYPE ratelimit,
#      GET WRONGTYPE; the window expiring; the TTL; the native rlhit verb;
#      bad arguments; a restart keeps the record, typed, still counting
#   2  an eager fleet of three: hits through ALL THREE nodes on one key are
#      counted in ONE table (the deciding node - a forwarded hit's reply
#      comes back on RESP and on the native door), and the record reaches
#      every node typed
#   3  the 0.4.4 gate: a member that does not advertise the type keeps
#      RL.HIT refused, naming it; when it leaves, RL.HIT works.  The older
#      member is the second argument, or - by default, so CI runs it - a
#      variant of THIS tree built without the ALIVE feature byte, which is
#      exactly the frame 0.4.3 sends (epochtest's way of testing two builds)
# Fail-first: before S314 there is no RL.HIT at all.
# Usage: test/ratelimittest.sh [./perfcached] [older-perfcached-for-part-3]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
OLD=${2:-}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18731 18732 18733 18741 18742 18743 18751 18752 18753; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "ratelimittest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcrl.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n> <cluster: yes|no>
	mkdir -p "$D/s$1/wal"
	if [ "$2" = yes ]; then CL="[cluster]
multicast = 239.255.80.31:18760
advertise = 127.0.38.$1
mode = eager
collections = 0"; else CL=; fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = rl-client-secret
cluster = rl-cluster-secret
[listen]
tcp = 127.0.0.1:1873$1
resp = 127.0.0.1:1874$1
http = 127.0.0.1:1875$1
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
start() { # start <n> <bin>
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
# r <n> <args...>: one RESP command to node n, the reply flattened to one line
r() { n=$1; shift; python3 - "1874$n" "$@" <<'PY'
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
    return v if t in ":+" else "ERR " + v
print(rd())
PY
}
# j <n> <method> <params json>: one native-door call (a CMD frame via
# pcnative), the result or error
j() { python3 - "1873$1" "$2" "$3" <<'PY'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": sys.argv[2], "params": json.loads(sys.argv[3])}) + "\n").encode()); f.flush()
m = json.loads(f.readline())
print(json.dumps(m.get("result", m.get("error")), sort_keys=True))
PY
}
fleet() { # every listed node sees all the others
	i=0; while [ $i -lt 120 ]; do
		good=1
		for x in "$@"; do
			n=$(curl -s "http://127.0.0.1:1875$x/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0))' 2>/dev/null)
			[ "$n" = "$#" ] || good=0
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}

# ---- 1. one node ----------------------------------------------------------
echo "part 1: one node"
conf 1 no; start 1 "$BIN" || exit 1
a="$(r 1 RL.HIT k1 2000 3) $(r 1 RL.HIT k1 2000 3) $(r 1 RL.HIT k1 2000 3) $(r 1 RL.HIT k1 2000 3)"
[ "$a" = "[1,1] [2,1] [3,1] [4,0]" ] && ok "1. a limit of 3 in 2 s: $a - the fourth hit is counted and refused" \
	|| bad "1. four hits, limit 3: $a"
[ "$(r 1 TYPE k1)" = ratelimit ] && [ "$(r 1 GET k1)" = "ERR WRONGTYPE Operation against a key holding the wrong kind of value" ] \
	&& ok "   TYPE ratelimit, GET WRONGTYPE" || bad "   TYPE $(r 1 TYPE k1), GET $(r 1 GET k1)"
t=$(r 1 TTL k1)
[ "$t" -ge 2 ] 2>/dev/null && [ "$t" -le 4 ] && ok "   TTL $t s - the window plus a margin" || bad "   TTL $t"
[ "$(r 1 RL.HIT k2 5000)" = "[1,1]" ] && [ "$(r 1 RL.HIT k2 5000)" = "[2,1]" ] \
	&& ok "   no limit: allowed is always 1" || bad "   no limit: $(r 1 RL.HIT k2 5000)"
sleep 2.2
b=$(r 1 RL.HIT k1 2000 3)
[ "$b" = "[1,1]" ] && ok "   2.2 s later the old hits fell out of the window: $b" || bad "   after the window: $b"
jr=$(j 1 rlhit '{"col":"0","key":"k3","window_ms":1000,"limit":1}')$(j 1 rlhit '{"col":"0","key":"k3","window_ms":1000,"limit":1}')
[ "$jr" = '{"allowed": true, "count": 1}{"allowed": false, "count": 2}' ] && ok "   native rlhit: $jr" || bad "   native rlhit: $jr"
e1=$(r 1 RL.HIT k1 0 3); e2=$(r 1 RL.HIT k1 1000 6000); e3=$(r 1 RL.HIT k1)
case "$e1$e2$e3" in ERR*ERR*ERR*) ok "   bad window, limit and arity refused";; *) bad "   bad args: $e1 | $e2 | $e3";; esac
r 1 SET s1 plain > /dev/null
[ "$(r 1 RL.HIT s1 1000 3)" = "ERR WRONGTYPE Operation against a key holding the wrong kind of value" ] \
	&& ok "   RL.HIT on a string: WRONGTYPE" || bad "   RL.HIT on a string: $(r 1 RL.HIT s1 1000 3)"
r 1 RL.HIT k4 60000 > /dev/null; r 1 RL.HIT k4 60000 > /dev/null
r 1 SYNC > /dev/null 2>&1; j 1 sync '{}' > /dev/null
stopn 1; start 1 "$BIN" || exit 1
[ "$(r 1 TYPE k4)" = ratelimit ] && [ "$(r 1 RL.HIT k4 60000)" = "[3,1]" ] \
	&& ok "   a restart keeps the record typed and counting: [3,1]" || bad "   after restart: TYPE $(r 1 TYPE k4), hit $(r 1 RL.HIT k4 60000)"
stopn 1

# ---- 2. an eager fleet of three -------------------------------------------
echo "part 2: an eager fleet of three"
for n in 1 2 3; do rm -rf "$D/s$n"; conf $n yes; done
for n in 1 2 3; do start $n "$BIN" || exit 1; done
fleet 1 2 3 || { bad "the fleet did not form"; echo "ratelimittest: $pass passed, $fail failed"; exit 1; }
sleep 2
out=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do n=$(( (i - 1) % 3 + 1 )); out="$out $(r $n RL.HIT shared 30000 10)"; done
want=" [1,1] [2,1] [3,1] [4,1] [5,1] [6,1] [7,1] [8,1] [9,1] [10,1] [11,0] [12,0]"
[ "$out" = "$want" ] && ok "2. twelve hits through nodes 1,2,3 in turn, limit 10: one count, 1..12, the 11th refused" \
	|| bad "2. twelve hits through three nodes:$out"
jn=$(j 2 rlhit '{"col":"0","key":"shared","window_ms":30000,"limit":10}')
[ "$jn" = '{"allowed": false, "count": 13}' ] && ok "   and the native door through node 2: $jn" || bad "   native through node 2: $jn"
sleep 1
ty="$(r 1 TYPE shared)$(r 2 TYPE shared)$(r 3 TYPE shared)"
[ "$ty" = ratelimitratelimitratelimit ] && ok "   the record reached every node, typed" || bad "   TYPE on 1/2/3: $ty"
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "   no ERROR/CRIT" || bad "   $e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
for n in 1 2 3; do stopn $n; done

# ---- 3. the 0.4.4 gate ----------------------------------------------------
if [ -z "$OLD" ]; then
	echo "--- building the no-features variant (the frame 0.4.3 sends)"
	mkdir -p "$D/t"
	tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
		(cd "$D/t" && tar -xf -) || { bad "cannot copy the tree"; exit 1; }
	# anchored on the exact text: if the source moves, the variant still
	# builds and the gate check below catches the variant that did not take
	sed -i '/pc_w8(&c, (unsigned)in->features);/d; s/buf\[len - 3\]/buf[len - 2]/' \
		"$D/t/src/clmemb.c"
	if make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.old.log" 2>&1; then
		OLD="$D/t/perfcached"
	else
		bad "the no-features variant did not build: $(tail -3 "$D/build.old.log" | tr '\n' ' ')"
	fi
fi
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
	echo "part 3: the gate, beside $("$OLD" -V | head -1)"
	for n in 1 2 3; do rm -rf "$D/s$n"; conf $n yes; done
	start 1 "$BIN" && start 2 "$BIN" && start 3 "$OLD" || exit 1
	fleet 1 2 3 || { bad "the mixed fleet did not form"; echo "ratelimittest: $pass passed, $fail failed"; exit 1; }
	sleep 2
	g=$(r 1 RL.HIT gk 10000 5)
	case "$g" in *"RL.HIT needs every node in the fleet on 0.4.4 or later - node "*) ok "3. with an older member: $g";;
		*) bad "3. with an older member RL.HIT answered: $g";; esac
	stopn 3
	i=0; while [ $i -lt 60 ]; do g=$(r 1 RL.HIT gk 10000 5); [ "$g" = "[1,1]" ] && break; sleep 0.5; i=$((i+1)); done
	[ "$g" = "[1,1]" ] && ok "   when it leaves, RL.HIT works: $g" || bad "   after the older member left: $g"
	for n in 1 2; do stopn $n; done
else
	echo "part 3: SKIP - no older build given (second argument)"
fi
echo "ratelimittest: $pass passed, $fail failed"
[ $fail -eq 0 ]
