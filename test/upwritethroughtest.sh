#!/bin/sh
# upwritethroughtest.sh - S385 (0.6.2): upstream_writes = through.  Three
# fakes (test/fakeupstream.py): M the master, accepting writes and copying
# its keyspace to R1 and R2, the replicas (which name M as their master).
# Collection 0 reads the replicas and writes through to M.
#  1. the writer finds M (ROLE), RedisJSON there (COMMAND INFO)
#  2. RESP: SET reaches M and never a replica; the fleet then holds what M
#     holds (read back): GET answers it, TTL is M's; SET NX on a key that
#     exists answers M's nil and changes nothing; DEL leaves it gone
#  3. JSON.SET ... EX: the document on M, then an EXPIRE (RedisJSON has no
#     EX); JSON.GET answers it here
#  4. the native door: set, setnx, expire, jset, del - forwarded, answered
#     as the verb's own reply from M's
#  5. unknown commands (upstream_unknown = pass): APPEND forwarded and the
#     key read back; LPUSH then LRANGE (a passed read, from a replica);
#     DEBUG (admin), BLPOP (blocking) refused; EVAL refused (perfcached's
#     own); and upstream_unknown = refuse keeps "unknown command"
#  6. M stops: writes refused ("no master reachable"), reads still served,
#     the alert write.0 critical;
#     R1 promoted: writes go there, found at once (no 10 s wait)
#  7. two masters: the one the replica follows; nobody followed: refused
#  8. no RedisJSON on the master: JSON writes refused here, others pass,
#     the alert write.0.json a warning
#  9. config: upstream_writes = through needs upstream; upstream_unknown
#     needs through
# Fail-first: 0.6.1 refuses upstream_writes = through at -C.
# Usage: test/upwritethroughtest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupwt.XXXXXX)
PM=19881 P1=19882 P2=19883 TCP=19884 RESP=19885 HTTP=19886
TCPB=19887 RESPB=19888 HTTPB=19889       # AR14: a second node
FM= F1= F2= PID= PIDB=
trap 'for p in $PID $PIDB $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $P2 $TCP $RESP $HTTP $TCPB $RESPB $HTTPB; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upwritethroughtest: port $p already bound" >&2; exit 1; }
done

ctl() { # ctl <m|1|2> <json>
	echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"
}
fakes() { # fakes: start M, R1, R2 with an empty keyspace
	for n in m 1 2; do
		rm -rf "$D/up$n"; mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"
	done
	ctl m '{"role": "master", "accept_writes": true, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
	ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
	ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
	python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
	python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
	python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
	i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
}
unfake() { for p in $FM $F1 $F2; do kill -9 $p 2>/dev/null; wait $p 2>/dev/null; done; FM= F1= F2=; }
conf() { # conf <name> [extra collection lines...]
	n=$1; shift
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = upwt-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 1000
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
NS=0
start() { # start <conf> - its log is $D/<conf>.<n>.log, the last one $D/<conf>.log
	rm -rf "$D/state"; mkdir -p "$D/state"
	NS=$((NS+1)); L="$D/$1.$NS.log"
	PERFCACHED_ALERT_FAST=60 "$BIN" -f "$D/$1.conf" > "$L" 2>&1 & PID=$!
	ln -sf "$L" "$D/$1.log"
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$L"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$L"
}
stop() { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
wt() { # wt <python expr over w (collection 0's write_through block)>
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
w=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]["write_through"]
print(eval(sys.argv[1]))' "$1" 2>"${DEBUG:-/dev/null}"
}
alert() { # alert <key> - its level, or none
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
print(next((a["level"] for a in s["alerts"]["active"] if a["key"]==sys.argv[1]), "none"))' "$1" 2>/dev/null
}
waitfor() { # waitfor <seconds> <shell condition>
	w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1
}
resp() { # resp <args...> - one command at the RESP door; prints its reply as python repr
	python3 - $RESP "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rb")
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
def rd():
    l = f.readline()
    t, body = l[:1], l[1:-2]
    if t == b"$":
        n = int(body)
        return None if n < 0 else f.read(n + 2)[:-2].decode("latin-1")
    if t == b"*":
        n = int(body)
        return None if n < 0 else [rd() for _ in range(n)]
    if t == b":":
        return int(body)
    return (t + body).decode("latin-1")
print(repr(rd()))
PY
}
mdata() { # mdata <dir> <key> <field> - a fake's stored entry field, or None
	python3 -c 'import json,sys
d=json.load(open(sys.argv[1]+"/data.json")); e=d.get(sys.argv[2])
print(None if e is None else e.get(sys.argv[3]))' "$D/up$1" "$2" "$3" 2>/dev/null
}
native() { # native <json request> - one CMD frame at the native door; prints the result
	python3 - $TCP "$1" "$T" <<'PY' 2>&1
import json, socket, sys
sys.path.insert(0, sys.argv[3])
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10))
r = json.loads(sys.argv[2]); r["jsonrpc"] = "2.0"; r["id"] = 1
r.setdefault("params", {})["col"] = "0"
f.write((json.dumps(r) + "\n").encode()); f.flush()
m = json.loads(f.readline())
print(json.dumps(m.get("result", m.get("error"))))
PY
}

# ---- 1. the writer finds the master -------------------------------------------------
fakes
conf a
if ! start a; then
	bad "the node did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a.log")"
	echo "upwritethroughtest: $pass passed, $fail failed"; exit 1
fi
waitfor 5 '[ "$(wt "w[\"target\"]")" = "127.0.0.1:'$PM'" ]'
[ "$(wt 'w["target"]')" = "127.0.0.1:$PM" ] && [ "$(wt 'w["json"]')" = yes ] &&
	ok "1. the writer found the master by ROLE, and RedisJSON there (COMMAND INFO)" ||
	bad "1. target $(wt 'w["target"]'), json $(wt 'w["json"]'), state $(wt 'w["state"]')"
grep -q "write-through to the master 127.0.0.1:$PM" "$D/a.log" && ok "1. the log names the write target" ||
	bad "1. log: $(grep -m2 'write-through' "$D/a.log")"

# ---- 2. RESP writes ---------------------------------------------------------------------
r=$(resp SET a:1 v1)
[ "$r" = "'+OK'" ] && [ "$(mdata m a:1 value)" = v1 ] && grep -q '^SET a:1 v1$' "$D/upm/writes" &&
	! grep -q 'SET' "$D/up1/writes" "$D/up2/writes" 2>/dev/null &&
	ok "2. SET reaches the master, never a replica (+OK)" || bad "2. SET: $r, M has $(mdata m a:1 value)"
[ "$(resp GET a:1)" = "'v1'" ] && ok "2. GET answers it here" || bad "2. GET: $(resp GET a:1)"
resp SET a:2 v2 EX 100 >/dev/null
t=$(resp TTL a:2)
[ "$t" -ge 95 ] 2>/dev/null && [ "$t" -le 100 ] && ok "2. SET ... EX 100: the TTL here is the master's ($t)" || bad "2. TTL a:2: $t"
r=$(resp SET a:1 other NX)
[ "$r" = "None" ] && [ "$(resp GET a:1)" = "'v1'" ] && ok "2. SET NX on a key that exists: the master's nil, nothing changed" ||
	bad "2. SET NX: $r, GET $(resp GET a:1)"
r=$(resp DEL a:1)
[ "$r" = "1" ] && [ "$(resp GET a:1)" = "None" ] && [ "$(mdata m a:1 value)" = None ] &&
	ok "2. DEL: the master's 1, gone there and here" || bad "2. DEL: $r, GET $(resp GET a:1), M $(mdata m a:1 value)"

# ---- 3. JSON.SET with perfcached's EX ------------------------------------------------------
r=$(resp JSON.SET d:1 '$' '{"x":1}' EX 50)
[ "$r" = "'+OK'" ] && grep -q '^JSON.SET d:1 \$ {"x":1}$' "$D/upm/writes" && grep -q '^EXPIRE d:1 50$' "$D/upm/writes" &&
	ok "3. JSON.SET ... EX 50: the document, then EXPIRE 50 (RedisJSON has no EX)" ||
	bad "3. JSON.SET: $r; M writes: $(grep 'd:1' "$D/upm/writes" | tr '\n' ' ')"
[ "$(resp JSON.GET d:1)" = "'{\"x\":1}'" ] && t=$(resp TTL d:1) && [ "$t" -ge 45 ] 2>/dev/null &&
	ok "3. JSON.GET answers the document here, TTL $t" || bad "3. JSON.GET: $(resp JSON.GET d:1), TTL $(resp TTL d:1)"

# ---- 4. the native door --------------------------------------------------------------------
r=$(native '{"method": "set", "params": {"key": "n:1", "value": "nv", "ttl": 60}}')
echo "$r" | grep -q '"stored": true' && [ "$(mdata m n:1 value)" = nv ] && grep -q '^SET n:1 nv EX 60$' "$D/upm/writes" &&
	ok "4. native set with a TTL: SET ... EX 60 on the master, {stored: true}" || bad "4. set: $r"
r=$(native '{"method": "setnx", "params": {"key": "n:1", "value": "nv2"}}')
echo "$r" | grep -q '"stored": false' && [ "$(mdata m n:1 value)" = nv ] &&
	ok "4. native setnx on a key that exists: {stored: false}, unchanged" || bad "4. setnx: $r"
r=$(native '{"method": "jset", "params": {"key": "n:2", "path": "$", "val": {"s": "Reachable"}, "ttl": 30}}')
echo "$r" | grep -q '"set": true' && grep -q '^JSON.SET n:2 \$ {"s":"Reachable"}$' "$D/upm/writes" &&
	ok "4. native jset (cachedb's JSON.SET): forwarded, {set: true}" || bad "4. jset: $r; M: $(grep 'n:2' "$D/upm/writes" | tr '\n' '|')"
r=$(native '{"method": "expire", "params": {"key": "n:1", "ttl": 0}}')
echo "$r" | grep -q '"updated": true' && grep -q '^PERSIST n:1$' "$D/upm/writes" &&
	ok "4. native expire with ttl 0 is PERSIST" || bad "4. expire: $r"
r=$(native '{"method": "del", "params": {"key": "n:1"}}')
echo "$r" | grep -q '"deleted": true' && [ "$(mdata m n:1 value)" = None ] &&
	ok "4. native del: {deleted: true}, gone on the master" || bad "4. del: $r"
r=$(native '{"method": "add", "params": {"key": "n:3", "by": 1}}')
echo "$r" | grep -qi 'does not forward' && ok "4. a verb not forwarded yet (add) is refused, named" || bad "4. add: $r"

# ---- 5. unknown commands --------------------------------------------------------------------
r=$(resp APPEND a:3 hello)
[ "$r" = "5" ] && [ "$(resp GET a:3)" = "'hello'" ] &&
	ok "5. APPEND (unknown here): the master's 5, and GET reads the value back here" ||
	bad "5. APPEND: $r, GET $(resp GET a:3)"
r=$(resp LPUSH l:1 a b)
r2=$(resp LRANGE l:1 0 -1)
[ "$r" = "2" ] && [ "$r2" = "['b', 'a']" ] &&
	ok "5. LPUSH, then LRANGE passed to a replica: $r2" || bad "5. LPUSH $r, LRANGE $r2"
grep -q '^LRANGE ' "$D/upm/counts" 2>/dev/null && bad "5. a passed READ reached the master" ||
	ok "5. the passed read never reached the master"
r=$(resp DEBUG OBJECT a:3); echo "$r" | grep -qi 'not passed - an admin' && ok "5. DEBUG (admin, unknown here) refused" || bad "5. DEBUG: $r"
r=$(resp BLPOP l:1 1); echo "$r" | grep -qi 'blocking' && ok "5. BLPOP (blocking) refused" || bad "5. BLPOP: $r"
r=$(resp EVAL 'return 1' 0); echo "$r" | grep -qi 'not forwarded' && ok "5. EVAL refused - it writes perfcached only" || bad "5. EVAL: $r"
w=$(wt '(w["passed_writes"], w["passed_reads"], w["pass_refused"])')
[ "$w" = "(2, 1, 2)" ] && ok "5. /stats: 2 passed writes, 1 passed read, 2 refused" || bad "5. pass counts: $w"

# ---- 5b. the write stats (0.6.2): by command, the last errors, one count a write --------------
r=$(resp HSET a:3 f v)                         # a string on the master: WRONGTYPE
echo "$r" | grep -q WRONGTYPE && ok "5b. HSET on a string: Redis's WRONGTYPE, relayed" || bad "5b. HSET: $r"
w=$(wt '{r["name"]: (r["applied"], r["not_applied"], r["redis_errors"], r["failed"], r["passed"]) for r in w["by_command"]}')
python3 -c 'import sys; d=eval(sys.argv[1]); sys.exit(0 if d.get("set")==(3,2,0,0,False) and d.get("del")==(2,0,0,0,False) and d.get("json.set")==(2,0,0,0,False) and d.get("persist")==(1,0,0,0,False) and d.get("append")==(1,0,0,0,True) and d.get("hset")==(0,0,1,0,False) else 1)' "$w" &&
	ok "5b. by command: set 3 applied + 2 not (NX), del 2, json.set 2, persist 1, append passed, hset 1 Redis error" ||
	bad "5b. by_command: $w"
w=$(wt '[(e["kind"], e["command"], e["key"], e["message"][:9]) for e in w["last_errors"]]')
python3 -c 'import sys; l=eval(sys.argv[1]); sys.exit(0 if l and l[0]==("redis","hset","a:3","WRONGTYPE") and ("refused","eval") in [x[:2] for x in l] and ("refused","cmd:add") in [x[:2] for x in l] else 1)' "$w" &&
	ok "5b. the last errors, newest first: hset a:3 WRONGTYPE; eval and cmd:add refused here" || bad "5b. last_errors: $w"
[ "$(wt 'w["win_max_us"] > 0 and w["by_command"][0]["n"] > 0')" = True ] && ok "5b. the slowest write in 5 min is kept" ||
	bad "5b. win_max_us $(wt 'w["win_max_us"]')"
c=$(python3 -c 'import json,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
d={(r["dialect"], r["verb"]): r["calls"] for r in s["commands"]}
print(d.get(("resp","set")), d.get(("cmd","set")), d.get(("cmd","setnx")))')
[ "$c" = "3 1 1" ] && ok "5b. a write that waited for the master is one call in the commands table (resp set 3, cmd set 1, setnx 1)" ||
	bad "5b. commands table calls (resp set, cmd set, cmd setnx): $c - a parked write counted twice?"
stop
conf b "upstream_unknown = refuse"
start b || bad "5. the node with upstream_unknown = refuse did not start"
r=$(resp APPEND a:3 x); echo "$r" | grep -qi 'unknown command' && ok "5. upstream_unknown = refuse: unknown command, as before" || bad "5. refuse: $r"
stop

# ---- 6. the master stops, a replica is promoted --------------------------------------------
start a || bad "6. restart"
waitfor 5 '[ "$(wt "w[\"target\"]")" = "127.0.0.1:'$PM'" ]'
resp SET b:1 v1 >/dev/null
ctl m '{"role": "master", "down": true}'
kill -9 $FM 2>/dev/null; wait $FM 2>/dev/null; FM=
resp SET b:2 x >/dev/null                      # finds the master gone
r=$(resp SET b:3 x)
echo "$r" | grep -qi 'no master reachable' && ok "6. the master gone: writes refused - $r" || bad "6. SET with no master: $r"
[ "$(resp GET b:1)" = "'v1'" ] && ok "6. reads are still served" || bad "6. GET b:1: $(resp GET b:1)"
waitfor 5 '[ "$(alert write.0)" = critical ]'
[ "$(alert write.0)" = critical ] && ok "6. the alert write.0 is critical: writes are refused" || bad "6. alert write.0: $(alert write.0)"
ctl 1 '{"role": "master", "accept_writes": true, "replicate_to": ["'"$D"'/up2"]}'
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$P1'}'
waitfor 12 '[ "$(resp SET b:4 promoted)" = "'"'"'+OK'"'"'" ]'
[ "$(mdata 1 b:4 value)" = promoted ] && [ "$(wt 'w["target"]')" = "127.0.0.1:$P1" ] &&
	ok "6. R1 promoted: writes go there" || bad "6. after the promotion: target $(wt 'w["target"]'), R1 b:4 $(mdata 1 b:4 value)"
stop

# ---- 7. two masters ---------------------------------------------------------------------------
unfake; fakes
ctl 1 '{"role": "master", "accept_writes": true}'
conf a
start a || bad "7. start"
waitfor 5 '[ "$(wt "w[\"target\"]")" != None ]'
[ "$(wt 'w["target"]')" = "127.0.0.1:$PM" ] && ok "7. two masters: the one the replica follows (M)" ||
	bad "7. target $(wt 'w["target"]'), state $(wt 'w["state"]')"
stop
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": 1}'
start a || bad "7. start"
sleep 2
[ "$(wt 'w["target"]')" = None ] && [ "$(wt 'w["state"]')" = "two masters, no replica names one" ] &&
	ok "7. nobody follows either: no target" || bad "7. target $(wt 'w["target"]'), state $(wt 'w["state"]')"
r=$(resp SET c:1 x); echo "$r" | grep -qi 'no master reachable' && ok "7. and writes are refused" || bad "7. SET: $r"
stop

# ---- 8. no RedisJSON ---------------------------------------------------------------------------
unfake; fakes
ctl m '{"role": "master", "accept_writes": true, "json": false}'
start a || bad "8. start"
waitfor 5 '[ "$(wt "w[\"json\"]")" = no ]'
r=$(resp JSON.SET d:2 '$' '{"y":2}')
echo "$r" | grep -qi 'no RedisJSON' && ! grep -q 'JSON.SET d:2' "$D/upm/writes" 2>/dev/null &&
	ok "8. no RedisJSON on the master: JSON.SET refused here, nothing sent" || bad "8. JSON.SET: $r"
[ "$(resp SET e:1 v)" = "'+OK'" ] && ok "8. other writes still go through" || bad "8. SET: $(resp SET e:1 v)"
grep -q "has no RedisJSON" "$D/a.log" && ok "8. the log says so" || bad "8. log"
waitfor 5 '[ "$(alert write.0.json)" = warning ]'
[ "$(alert write.0.json)" = warning ] && ok "8. the alert write.0.json is a warning" || bad "8. alert write.0.json: $(alert write.0.json)"
stop
unfake

# ---- 10. a slow master: the queue is bounded, a write's deadline runs from its queueing (AR2) ---
# The master answers every write 3 s late; upstream_timeout_ms is 1 s and at most 10 writes may
# wait.  40 writes at once: the one being sent times out ("may or may not"), the ~9 queued behind
# it are answered "not applied" - under a quarter of their second left, never sent - and the rest
# are refused at once.  On 0.6.2 nothing was refused and every queued write waited its turn.
fakes
ctl m '{"role": "master", "accept_writes": true, "write_delay_ms": 3000, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
conf q "upstream_write_queue = 10"
start q || bad "10. the node with upstream_write_queue = 10 did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/q.log")"
waitfor 5 '[ "$(wt "w[\"target\"]")" = "127.0.0.1:'$PM'" ]'
r=$(python3 - $RESP <<'PY'
import socket, sys, time, threading
port = int(sys.argv[1]); res = []
def one(i):
    t = time.time()
    try:
        s = socket.create_connection(("127.0.0.1", port), 10)
        s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\nq:%d\r\n$1\r\nv\r\n" % (len("q:%d" % i), i))
        line = s.makefile("rb").readline().decode("latin-1", "replace").strip()
    except Exception as e:
        line = "EXC %s" % e
    res.append((time.time() - t, line))
th = [threading.Thread(target=one, args=(i,)) for i in range(40)]
for x in th: x.start()
for x in th: x.join()
full = sum("queue is full" in l for _, l in res)
expired = sum("waited too long for the master" in l for _, l in res)
maybe = sum("may or may not" in l for _, l in res)
print(full, expired, maybe, sum(l.startswith("+OK") for _, l in res), "%.1f" % max(t for t, _ in res))
PY
)
set -- $r
[ "${1:-0}" -ge 20 ] && [ "${2:-0}" -ge 5 ] && [ "${4:-1}" = 0 ] && ok "10. 40 writes at a 3 s master: $1 refused at once (the queue is full), $2 answered not applied after their deadline, $3 may-or-may-not, none OK" ||
	bad "10. full $1, expired $2, maybe $3, ok $4 (want >= 20 full, >= 5 expired, 0 OK)"
awk -v m="${5:-99}" 'BEGIN { exit !(m < 2.5) }' && ok "10. no client waited longer than its timeout and a half (slowest ${5}s)" ||
	bad "10. the slowest reply took ${5}s - writes waited in line past their deadline"
n=$(grep -c '^SET q:' "$D/upm/writes" 2>/dev/null)
[ "${n:-9}" -le 2 ] && ok "10. the master saw $n of the 40 - the expired and the refused were never sent" || bad "10. the master saw $n SETs"
w=$(wt '(w["queue_max"], w["queue_full"] >= 20, w["queue_expired"] >= 5)')
[ "$w" = "(10, True, True)" ] && ok "10. /stats: queue_max 10, the refusals and the expiries counted" || bad "10. /stats: $w"
waitfor 15 '[ "$(alert write.0.queue)" = warning ]'
[ "$(alert write.0.queue)" = warning ] && ok "10. the alert write.0.queue is a warning" || bad "10. alert write.0.queue: $(alert write.0.queue)"
stop
unfake

# ---- 11. another Redis client's changes are seen (AR1) ------------------------------------------
# Broadcast tracking: a key written through here, then changed DIRECTLY on the master by another
# client, must read the new value through perfcached; deleted there, it must read as gone.  A key
# outside upstream_prefixes is the fleet's own and is not touched.  On 0.6.2 the record written
# through was the fleet's for good: the other client's value never showed.
rawm() { # rawm <args...> - one command straight to the fake MASTER, as another Redis client
	python3 - $PM "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
print(s.makefile("rb").readline().decode("latin-1").strip())
PY
}
fakes
conf o "upstream_prefixes = o:*"
sed -i 's/^upstream_prefixes = \*$//' "$D/o.conf"
start o || bad "11. start: $(grep -m2 -E 'ERROR|CRIT' "$D/o.log")"
waitfor 5 '[ "$(wt "w[\"target\"]")" = "127.0.0.1:'$PM'" ] && [ "$(wt "w[\"bcast\"]")" = on ]'
[ "$(wt 'w["bcast"]')" = on ] && grep -q 'TRACKING ON BCAST PREFIX o:' "$D/up1/counts" &&
	ok "11. broadcast tracking on, asked for the prefix o: (upstream_prefixes = o:*)" ||
	bad "11. bcast $(wt 'w["bcast"]'); R1 heard: $(grep TRACKING "$D/up1/counts" | tail -2 | tr '\n' '|')"
r=$(resp SET o:1 mine); r2=$(resp SET x:1 ours)
[ "$r" = "'+OK'" ] && [ "$(resp GET o:1)" = "'mine'" ] && ok "11. o:1 written through: 'mine'" || bad "11. SET o:1: $r, GET $(resp GET o:1)"
r=$(rawm SET o:1 theirs)
waitfor 5 '[ "$(resp GET o:1)" = "'"'"'theirs'"'"'" ]'
[ "$(resp GET o:1)" = "'theirs'" ] && ok "11. another client's SET on the master: perfcached now reads 'theirs'" ||
	bad "11. after another client's SET ($r): GET o:1 = $(resp GET o:1) - still the record written through"
r=$(rawm DEL o:1)
waitfor 5 '[ "$(resp GET o:1)" = None ]'
[ "$(resp GET o:1)" = None ] && ok "11. another client's DEL: perfcached reads o:1 as gone" || bad "11. after DEL ($r): GET o:1 = $(resp GET o:1)"
rawm SET x:1 theirs > /dev/null
sleep 1
[ "$(resp GET x:1)" = "'ours'" ] && ok "11. x:1 (outside upstream_prefixes) is the fleet's own: still 'ours'" || bad "11. x:1 = $(resp GET x:1)"
# the SET dropped the record written through (1); the DEL found o:1 only in the read-through shadow,
# which on_push clears without counting - "invalidated" counts records written through
[ "$(wt 'w["invalidated"] >= 1')" = True ] && ok "11. /stats: the record written through, dropped on Redis's word, counted" || bad "11. invalidated $(wt 'w["invalidated"]')"
# a gap in the tracking: every key written through goes (it may have missed a change)
resp SET o:2 v2 > /dev/null; resp GET o:2 > /dev/null
f0=$(wt 'w["inval_flushes"]')
ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM', "kill_seq": 1}'
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM', "kill_seq": 1}'
waitfor 8 '[ "$(wt "w[\"inval_flushes\"] > '$f0'")" = True ]'
[ "$(wt "w['inval_flushes'] > $f0")" = True ] &&
	ok "11. a tracking connection lost: the keys written through are dropped here (a flush counted)" ||
	bad "11. after the tracking was cut: inval_flushes $(wt 'w["inval_flushes"]') (was $f0)"
[ "$(resp GET o:2)" = "'v2'" ] && ok "11. and o:2 reads Redis's value again" || bad "11. GET o:2 = $(resp GET o:2)"
stop
unfake

# ---- 12. a delete Redis confirms is a tombstone even where the key is not held (AR14) -----------
# Two eager nodes writing through.  A DEL of a key no node holds goes to Redis, which says gone; the
# deleting node must still send a tombstone at a fresh version - a SET of the key pushed from the
# other node may be on its way, and without the tombstone it would land and stay.  On 0.6.2 the
# node removed nothing and sent nothing: its tomb_sent did not move.
cnode() { # cnode <name> <n> <tcp> <resp> <http>
	{ cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$2
[memory]
arena_mb = 64
[secrets]
client = upwt-client-secret
cluster = upwt-cluster-secret
[listen]
tcp = 127.0.0.1:$3
resp = 127.0.0.1:$4
http = 127.0.0.1:$5
plaintext = loopback
[cluster]
multicast = 239.255.78.23:19890
advertise = 127.0.82.$2
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 1000
C
	} > "$D/$1.conf"; chmod 600 "$D/$1.conf"
	rm -rf "$D/state$2"; mkdir -p "$D/state$2"
}
cst() { # cst <http port> <python expr over s (the whole /stats)>
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null
}
respb() { _r=$RESP; RESP=$RESPB; resp "$@"; RESP=$_r; }   # the same, at node B
fakes
cnode ca 1 $TCP $RESP $HTTP; cnode cb 2 $TCPB $RESPB $HTTPB
PERFCACHED_ALERT_FAST=60 "$BIN" -f "$D/ca.conf" > "$D/ca.log" 2>&1 & PID=$!
PERFCACHED_ALERT_FAST=60 "$BIN" -f "$D/cb.conf" > "$D/cb.log" 2>&1 & PIDB=$!
waitfor 30 '[ "$(cst $HTTP "s[\"cluster\"][\"peers_up\"]")" = 1 ] && [ "$(cst $HTTPB "s[\"cluster\"][\"peers_up\"]")" = 1 ]'
[ "$(cst $HTTPB 's["cluster"]["peers_up"]')" = 1 ] && ok "12. two eager nodes writing through, joined" ||
	bad "12. the fleet did not form: A $(cst $HTTP 's["cluster"]["peers_up"]'), B $(cst $HTTPB 's["cluster"]["peers_up"]')"
waitfor 5 '[ "$(cst $HTTPB "[c for c in s[\"collections\"] if c[\"name\"]==\"0\"][0][\"upstream\"][\"write_through\"][\"target\"]")" = "127.0.0.1:'$PM'" ]'
t0=$(cst $HTTPB 's["cluster"]["tomb_sent"]')
r=$(respb DEL z:absent)
waitfor 5 '[ "$(cst $HTTPB "s[\"cluster\"][\"tomb_sent\"] > '"${t0:-0}"'")" = True ]'
[ "$r" = "0" ] && [ "$(cst $HTTPB "s['cluster']['tomb_sent'] > ${t0:-0}")" = True ] &&
	ok "12. DEL of a key no node holds: Redis says 0, and node B still sent a tombstone" ||
	bad "12. DEL z:absent: $r; B's tomb_sent $(cst $HTTPB 's["cluster"]["tomb_sent"]') (was $t0)"
# the race it is for, as near as a test can get: SET through A, DEL through B at once
python3 - $RESP $RESPB <<'PY'
import socket, sys
a = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
b = socket.create_connection(("127.0.0.1", int(sys.argv[2])), 5)
a.sendall(b"*3\r\n$3\r\nSET\r\n$3\r\nz:1\r\n$1\r\nv\r\n")
b.sendall(b"*2\r\n$3\r\nDEL\r\n$3\r\nz:1\r\n")
a.makefile("rb").readline(); b.makefile("rb").readline()
PY
sleep 2
ga=$(resp GET z:1); gb=$(respb GET z:1); gm=$(mdata m z:1 value)
agree=no
if [ "$ga" = "$gb" ]; then
	if [ "$gm" = None ]; then [ "$ga" = None ] && agree=yes; else [ "$ga" != None ] && agree=yes; fi
fi
[ $agree = yes ] && ok "12. SET through A and DEL through B at once: both nodes agree with Redis ($ga, Redis $gm)" ||
	bad "12. after SET via A + DEL via B: A $ga, B $gb, Redis $gm"
kill $PID $PIDB 2>/dev/null; wait $PID $PIDB 2>/dev/null; PID= PIDB=
unfake

# ---- 9. config --------------------------------------------------------------------------------
cfgerr() { # cfgerr <name> <expected> <collection lines...>
	n=$1 msg=$2; shift 2
	{ printf '[daemon]\nstate_dir = %s/state\n[secrets]\nclient = x\n[listen]\ntcp = 127.0.0.1:%s\nplaintext = loopback\n[collection 0]\nbuckets_log2 = 8\n' "$D" $TCP
	  for l in "$@"; do echo "$l"; done; } > "$D/$n.conf"; chmod 600 "$D/$n.conf"
	"$BIN" -C -f "$D/$n.conf" > "$D/$n.out" 2>&1
	[ $? -ne 0 ] && grep -qF "$msg" "$D/$n.out"
}
cfgerr c1 "upstream_writes needs upstream" "upstream_writes = through" &&
	ok "9. upstream_writes = through without an upstream is refused" || bad "9. c1: $(tail -1 "$D/c1.out")"
cfgerr c2 "upstream_unknown needs upstream_writes = through" "upstream = redis://127.0.0.1:$PM" "upstream_prefixes = *" "upstream_unknown = pass" &&
	ok "9. upstream_unknown without write-through is refused" || bad "9. c2: $(tail -1 "$D/c2.out")"
cfgerr c3 "upstream_write_queue needs upstream_writes = through" "upstream = redis://127.0.0.1:$PM" "upstream_prefixes = *" "upstream_write_queue = 100" &&
	ok "9. upstream_write_queue without write-through is refused (AR2)" || bad "9. c3: $(tail -1 "$D/c3.out")"

echo "upwritethroughtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
