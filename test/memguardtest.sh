#!/bin/sh
# memguardtest.sh - S366: the memory guard refuses new client connections
# while less than the floor stays free, the way max_clients refuses (Redis's
# exact text on RESP, an error frame on the native door), counts and logs
# it once a spell, keeps every held connection and the HTTP door answering,
# leaves the refusing state only past the hysteresis, and is off when
# memory_floor = off.  max_clients' default counts memory.  The limit is
# read from /proc/meminfo, cgroup v2 and cgroup v1 - fed here through the
# test hooks PERFCACHED_TEST_MEMINFO / _PROC_CGROUP / _CGROUP_ROOT.
# Fail-first: 0.5.6.4 has no guard, no hooks and no clients.memory.
# Usage: test/memguardtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcmemguard.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MB=1048576

# a meminfo with MemTotal 1 GB (kB, as the kernel writes it)
meminfo() { printf 'MemTotal:        1048576 kB\nMemFree:          %d kB\nMemAvailable:     %d kB\n' "$1" "$1" > "$D/meminfo.tmp" && mv "$D/meminfo.tmp" "$D/meminfo"; }

conf() {   # conf <name> <floor line or empty> <port base>
	mkdir -p "$D/state.$1"
	cat > "$D/$1.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/state.$1
$2
[memory]
arena_mb = 32
[secrets]
client = memguard-client-secret
[listen]
tcp = 127.0.0.1:$(($3 + 1))
resp = 127.0.0.1:$(($3 + 2))
http = 127.0.0.1:$(($3 + 3))
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/$1.conf"
}

# the soft descriptor limit in a CI job is 1024 (GitLab 110082): raise it to the
# hard one, so the memory-derived limit is the smaller one wherever that allows
ulimit -n "$(ulimit -H -n)" 2>/dev/null
start() {  # start <name> - env from the caller
	"$BIN" -f "$D/$1.conf" -D > "$D/$1.log" 2>&1 & P=$!
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  daemon $1 did not start:"; tail -5 "$D/$1.log"; return 1
}
stop() { [ -n "$P" ] && kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=; }

# ---- A: meminfo only (no cgroup files under the fake root) -----------------
mkdir -p "$D/cgA"; echo "0::/nothing/here" > "$D/cgroupA"
meminfo 600000                                 # ~586 MB available
conf a "" 17390
# the hooks: exported - a VAR=x prefix on a FUNCTION call does not reach
# the commands inside it under dash
export PERFCACHED_TEST_MEMINFO=$D/meminfo PERFCACHED_TEST_PROC_CGROUP=$D/cgroupA PERFCACHED_TEST_CGROUP_ROOT=$D/cgA
start a || { echo "memguardtest: $pass passed, 1 failed"; exit 1; }
grep -q "memory: TEST HOOK - /proc/meminfo read from" "$D/a.log" && ok "the meminfo hook is logged when used" || bad "no log line for the meminfo hook"
grep -q "memory: limit 1024 MB (meminfo)" "$D/a.log" && ok "boot names the limit and where it came from" || bad "no 'memory: limit 1024 MB (meminfo)' line: $(grep 'memory: limit' "$D/a.log")"
grep -q "floor 153 MB (memory_floor 15%)" "$D/a.log" && ok "the default floor is 15% of the limit (153 MB of 1024)" || bad "floor line: $(grep -o 'floor [0-9]* MB ([^)]*)' "$D/a.log")"
# max_clients = min(descriptors, memory), and the line names the smaller - the
# descriptor figure is the limit less the reserve, both on the same line
LIM=$(grep -o 'clients: max_clients [0-9]* ([^)]*), descriptor limit [0-9]*, reserve [0-9]*, memory for ~[0-9]* idle' "$D/a.log")
set -- $(echo "$LIM" | sed -E 's/.*max_clients ([0-9]+) \(derived from ([a-z]+)\), descriptor limit ([0-9]+), reserve ([0-9]+), memory for ~([0-9]+) idle/\1 \2 \3 \4 \5/')
if [ $# -eq 5 ]; then
	FD=$(($3 - $4)); WANT=memory; [ "$FD" -lt "$5" ] && WANT=descriptors
	[ "$2" = "$WANT" ] && [ "$1" -eq "$( [ "$FD" -lt "$5" ] && echo "$FD" || echo "$5")" ] \
		&& ok "max_clients $1 = min(descriptors $FD, memory $5), and the line says '$2'" \
		|| bad "max_clients $1 derived from $2: want $WANT (descriptors $FD, memory $5)"
	[ "$WANT" = memory ] || echo "  note descriptor limit $3 here - the memory basis is not exercised"
	DESC=$FD
else
	bad "limit line: $(grep 'clients: max_clients' "$D/a.log")"; DESC=0
fi

timeout 90 python3 - "$D" "$DESC" <<'PY' > "$D/py.out" 2>&1
import json, os, pcnative, socket, sys, time, urllib.request
D = sys.argv[1]; DESC = int(sys.argv[2]); res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
def st(): return json.load(urllib.request.urlopen("http://127.0.0.1:17393/stats", timeout=5))
def resp(s, *args):
    s.sendall(("*%d\r\n" % len(args)).encode() + b"".join(("$%d\r\n%s\r\n" % (len(a), a)).encode() for a in args))
    return s.recv(256)
def meminfo(kb):
    open(D + "/meminfo.tmp", "w").write("MemTotal:        1048576 kB\nMemFree:          %d kB\nMemAvailable:     %d kB\n" % (kb, kb))
    os.rename(D + "/meminfo.tmp", D + "/meminfo")
def until(pred, secs=5):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v: return v
        time.sleep(0.2)
    return pred()
def drain(s):
    d = b""
    try:
        while True:
            b = s.recv(256)
            if not b: break
            d += b
    except Exception:
        pass
    return d
c = st()["clients"]; m = c.get("memory", {})
ok(m.get("guard") is True and m.get("source") == "meminfo" and m.get("limit") == 1024 << 20,
   "/stats clients.memory: guard on, source meminfo, limit 1 GB (got %r)" % {k: m.get(k) for k in ("guard", "source", "limit")})
ok(m.get("floor") == (1024 << 20) // 100 * 15, "floor = 15%% of the limit (got %r)" % m.get("floor"))
mm = m.get("max_clients", -1)
want = ("memory", mm) if not DESC or mm <= DESC else ("descriptors", DESC)
ok(0 < mm < 200000 and (c.get("max_basis"), c.get("max")) == want,
   "clients.max = min(descriptors, memory) with its basis (max %r, memory.max_clients %r, descriptors %r, basis %r)" % (c.get("max"), mm, DESC, c.get("max_basis")))
ok(m.get("refusing") is False and m.get("budget", 0) > 0, "with ~586 MB available there is room (budget %r)" % m.get("budget"))
held = socket.create_connection(("127.0.0.1", 17392), timeout=5)
ok(resp(held, "PING") == b"+PONG\r\n", "a RESP connection opened before the squeeze answers")
# the squeeze: 100 MB available, under the 153 MB floor
meminfo(102400)
v = until(lambda: st()["clients"]["memory"]["budget"] <= 0, 4)
ok(v, "within a sample the budget is spent (budget %r)" % st()["clients"]["memory"].get("budget"))
s = socket.create_connection(("127.0.0.1", 17392), timeout=5)
d = drain(s); s.close()
ok(d == b"-ERR max number of clients reached\r\n", "a new RESP connection gets Redis's exact refusal, then EOF (got %r)" % d)
j = socket.create_connection(("127.0.0.1", 17391), timeout=5)
jf = pcnative.wrap(j)
jf.write(b'{"jsonrpc":"2.0","id":9,"method":"ping"}\n'); jf.flush()
try:
    first = jf.readline(); rest = jf.readline()
    r = json.loads(first) if first else {}
    ok("max number of clients" in r.get("error", {}).get("message", "") and r.get("id") == 9 and rest == b"",
       "a native client is refused at its first request with the error frame and its id, then EOF (got %r)" % first[:100])
except Exception as e:
    ok(False, "a native client is refused at its first request (got %r)" % (e,))
ok(resp(held, "PING") == b"+PONG\r\n", "the held connection still answers while new ones are refused")
c = st()["clients"]; m = c["memory"]
ok(m.get("refusing") is True and m.get("refused") == 2 and c.get("refused") == 2 and m.get("engaged") == 1,
   "/stats answers in the squeeze: refusing, 2 refused at the floor, inside clients.refused 2, one spell (%r)" % {k: m.get(k) for k in ("refusing", "refused", "engaged")})
mt = urllib.request.urlopen("http://127.0.0.1:17393/metrics", timeout=5).read().decode()
ok("perfcached_memory_refusing 1" in mt and "perfcached_clients_refused_memory_total 2" in mt and
   "perfcached_memory_floor_bytes %d" % m["floor"] in mt, "/metrics carries the state, the floor and the refusals")
# just above floor + arena reserve, under the hysteresis: still refusing
need = (m["floor"] + m["arena_reserve"]) // 1024
meminfo(need + 20 * 1024)                      # +20 MB, hysteresis is 51 MB
time.sleep(2.5)
s = socket.create_connection(("127.0.0.1", 17392), timeout=5); d = drain(s); s.close()
ok(d == b"-ERR max number of clients reached\r\n" and st()["clients"]["memory"]["refusing"] is True,
   "20 MB above the floor is inside the 5%% hysteresis: still refusing (got %r)" % d)
meminfo(600000)
v = until(lambda: not st()["clients"]["memory"]["refusing"], 4)
ok(v, "with ~586 MB available again the guard stops refusing")
s = socket.create_connection(("127.0.0.1", 17392), timeout=5)
ok(resp(s, "PING") == b"+PONG\r\n", "and a new connection is admitted")
s.close(); held.close()
for k, m in res: print("  %-4s %s" % (k, m))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the python driver did not finish: $(tail -3 $D/py.out)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + ${pf:-0}))
sleep 0.3
[ "$(grep -c 'clients: memory floor reached' "$D/a.log")" = 1 ] && ok "one WARNING for the whole spell" || bad "$(grep -c 'clients: memory floor reached' "$D/a.log") WARNING line(s), want 1"
grep -q "clients: memory above the floor again" "$D/a.log" && ok "one NOTICE when it cleared, with the count" || bad "no NOTICE when the guard cleared"
grep -q "disconnected (memory floor)" "$D/a.log" && ok "the native refusal's close names the floor" || bad "no 'disconnected (memory floor)' line"
stop

# ---- B: memory_floor = off - nothing refuses ------------------------------
printf 'MemTotal:        1048576 kB\nMemAvailable:     51200 kB\n' > "$D/meminfo"
conf b "memory_floor = off" 17394
# the hooks: exported - a VAR=x prefix on a FUNCTION call does not reach
# the commands inside it under dash
export PERFCACHED_TEST_MEMINFO=$D/meminfo PERFCACHED_TEST_PROC_CGROUP=$D/cgroupA PERFCACHED_TEST_CGROUP_ROOT=$D/cgA
start b || bad "daemon b did not start"
grep -q "the client guard is OFF" "$D/b.log" && ok "memory_floor = off says so at boot" || bad "no 'guard is OFF' line"
r=$(timeout 10 python3 -c "
import socket; s = socket.create_connection(('127.0.0.1', 17396), timeout=5); s.sendall(b'*1\r\n\$4\r\nPING\r\n'); print(s.recv(64))")
[ "$r" = "b'+PONG\\r\\n'" ] && ok "with the guard off, 50 MB available still admits (got $r)" || bad "guard off: got $r"
stop

# ---- C: cgroup v2 - the tightest level binds; headroom = max - current + inactive_file
mkdir -p "$D/cg2/svc.slice/pc.service"
echo "max" > "$D/cg2/svc.slice/pc.service/memory.max"
echo 734003200 > "$D/cg2/svc.slice/pc.service/memory.current"
printf 'anon 1\ninactive_file 1\n' > "$D/cg2/svc.slice/pc.service/memory.stat"
echo 805306368 > "$D/cg2/svc.slice/memory.max"                 # 768 MB
echo 524288000 > "$D/cg2/svc.slice/memory.current"             # 500 MB used
printf 'anon 400000000\nfile 100\ninactive_file 10485760\nactive_file 5\n' > "$D/cg2/svc.slice/memory.stat"   # 10 MB
echo "0::/svc.slice/pc.service" > "$D/cgroup2"
printf 'MemTotal:        4194304 kB\nMemAvailable:     3145728 kB\n' > "$D/meminfo"
conf c "" 17394
# the hooks: exported - a VAR=x prefix on a FUNCTION call does not reach
# the commands inside it under dash
export PERFCACHED_TEST_MEMINFO=$D/meminfo PERFCACHED_TEST_PROC_CGROUP=$D/cgroup2 PERFCACHED_TEST_CGROUP_ROOT=$D/cg2
start c || bad "daemon c did not start"
grep -q "memory: limit 768 MB (cgroup v2 /svc.slice)" "$D/c.log" && ok "cgroup v2: the limited level binds and is named" || bad "v2 line: $(grep 'memory: limit' "$D/c.log")"
r=$(timeout 10 python3 -c "
import json, urllib.request; m = json.load(urllib.request.urlopen('http://127.0.0.1:17397/stats', timeout=5))['clients']['memory']
print(m['limit'], m['available'], m['floor'])")
[ "$r" = "805306368 $((805306368 - 524288000 + 10485760)) 134217728" ] && ok "v2 headroom = max - current + inactive_file; floor 128 MB minimum ($r)" || bad "v2 figures: $r"
stop

# ---- D: cgroup v1 ----------------------------------------------------------
mkdir -p "$D/cg1/memory/grp"
echo 9223372036854771712 > "$D/cg1/memory/memory.limit_in_bytes"   # v1's "unlimited" root
echo 1 > "$D/cg1/memory/memory.usage_in_bytes"
echo 1073741824 > "$D/cg1/memory/grp/memory.limit_in_bytes"
echo 524288000 > "$D/cg1/memory/grp/memory.usage_in_bytes"
printf 'cache 1\ninactive_file 7\ntotal_inactive_file 20971520\n' > "$D/cg1/memory/grp/memory.stat"
printf '12:pids:/x\n5:cpu,cpuacct:/y\n4:memory:/grp\n0::/\n' > "$D/cgroup1"
conf d "memory_floor = 200 MB" 17394
# the hooks: exported - a VAR=x prefix on a FUNCTION call does not reach
# the commands inside it under dash
export PERFCACHED_TEST_MEMINFO=$D/meminfo PERFCACHED_TEST_PROC_CGROUP=$D/cgroup1 PERFCACHED_TEST_CGROUP_ROOT=$D/cg1
start d || bad "daemon d did not start"
grep -q "memory: limit 1024 MB (cgroup v1 /grp)" "$D/d.log" && ok "cgroup v1: the memory controller's path, its limit named" || bad "v1 line: $(grep 'memory: limit' "$D/d.log")"
grep -q "(memory_floor 200 MB)" "$D/d.log" && ok "memory_floor = 200 MB is taken exactly" || bad "MB floor: $(grep -o 'memory_floor [^)]*' "$D/d.log")"
r=$(timeout 10 python3 -c "
import json, urllib.request; m = json.load(urllib.request.urlopen('http://127.0.0.1:17397/stats', timeout=5))['clients']['memory']
print(m['available'], m['floor'])")
[ "$r" = "$((1073741824 - 524288000 + 20971520)) $((200 * MB))" ] && ok "v1 headroom uses total_inactive_file ($r)" || bad "v1 figures: $r"
stop

# ---- E: the key's syntax ----------------------------------------------------
for v in "60%" "0%" "abc" "5 TB" "2000 GB"; do
	conf e "memory_floor = $v" 17394
	"$BIN" -C -f "$D/e.conf" > "$D/e.out" 2>&1 && bad "memory_floor = $v accepted" || {
		grep -q "memory_floor:" "$D/e.out" && ok "memory_floor = $v refused, naming the key" || bad "memory_floor = $v: $(tail -1 "$D/e.out")"; }
done
for v in "25%" "512 MB" "2 GB" "off"; do
	conf e "memory_floor = $v" 17394
	"$BIN" -C -f "$D/e.conf" > "$D/e.out" 2>&1 && ok "memory_floor = $v accepted" || bad "memory_floor = $v refused: $(tail -1 "$D/e.out")"
done
echo "memguardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
