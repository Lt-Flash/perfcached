#!/bin/sh
# grafanatest.sh — the three observability surfaces the Grafana Redis
# datasource renders its panels from (S53): INFO (summary fields +
# commandstats), CLIENT LIST, SLOWLOG.
#
# The formats here are not ours to design - the plugin parses Redis's
# exact shapes, so every assertion below checks the SHAPE as well as
# the value: cmdstat_<name>:calls=N,usec=N,usec_per_call=F, CLIENT
# LIST as space-separated k=v lines, SLOWLOG entries as 6-field arrays
# (id, unix-time, usec, argv-array, addr, name).
#
# Counts are asserted EXACTLY where possible: the driver is a raw RESP
# client, so nothing injects hidden commands the way redis-cli does.
# Usage: test/grafanatest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcgf.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkconf() { # mkconf <file> <slowlog_usec-line>
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = notice
$2
[memory]
arena_mb = 64
[secrets]
client = gf-client-secret
cluster = gf-cluster-secret
[listen]
tcp = 127.0.0.1:17571
resp = 127.0.0.1:17572
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
	chmod 600 "$1"
}

start() { # start <conf> <log> -> pid via $STARTED
	"$BIN" -f "$1" > "$2" 2>&1 &
	STARTED=$!
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$2" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; cat "$2"; return 1
}

mkconf "$D/a.conf" "slowlog_usec = 0"
start "$D/a.conf" "$D/a.log" || exit 1
P1=$STARTED

python3 - "$D" <<'EOF'
import socket, sys, time

D = sys.argv[1]
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def conn():
    s = socket.create_connection(("127.0.0.1", 17572), 8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(10)
    return s

def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out

def parse(f):
    h = f.readline()
    t, body = h[:1], h[1:].rstrip(b"\r\n")
    if t == b":": return int(body)
    if t in (b"+", b"-"): return body.decode()
    if t == b"$":
        n = int(body)
        if n < 0: return None
        d = f.read(n + 2)[:-2]
        return d
    if t == b"*":
        return [parse(f) for _ in range(int(body))]
    raise ValueError(h)

def call(s, f, args):
    s.sendall(cmd(args))
    return parse(f)

a = conn(); fa = a.makefile("rwb")
b = conn(); fb = b.makefile("rwb")

# ---- exact-count traffic: 40 GET, 25 SET, on conn a ----
for i in range(25):
    call(a, fa, [b"SET", b"gk:%d" % i, b"v"])
for i in range(40):
    call(a, fa, [b"GET", b"gk:%d" % (i % 25)])
call(b, fb, [b"CLIENT", b"SETNAME", b"grafana-probe"])

# ---- INFO: summary fields the datasource reads ----
info = call(a, fa, [b"INFO"])
if not isinstance(info, bytes):
    bad("INFO did not return a bulk: %r" % (info,)); info = b""
txt = info.decode(errors="replace")
def field(name):
    for ln in txt.splitlines():
        if ln.startswith(name + ":"):
            return ln.split(":", 1)[1].strip()
    return None
cc = field("connected_clients")
if cc and int(cc) >= 2:
    ok("connected_clients=%s (>=2)" % cc)
else:
    bad("connected_clients missing or <2: %r" % cc)
um = field("used_memory")
if um and int(um) > 0:
    ok("used_memory=%s (>0)" % um)
else:
    bad("used_memory missing or 0: %r" % um)
tc = field("total_commands_processed")
if tc and int(tc) >= 65:
    ok("total_commands_processed=%s (>=65)" % tc)
else:
    bad("total_commands_processed missing or low: %r" % tc)
for f2 in ("instantaneous_ops_per_sec", "keyspace_hits", "keyspace_misses"):
    if field(f2) is not None:
        ok("INFO carries %s" % f2)
    else:
        bad("INFO lacks %s" % f2)

# ---- INFO commandstats: exact calls, plugin-exact shape ----
cs = call(a, fa, [b"INFO", b"commandstats"])
cst = cs.decode(errors="replace") if isinstance(cs, bytes) else ""
import re
mget = re.search(r"cmdstat_get:calls=(\d+),usec=(\d+),usec_per_call=([0-9.]+)", cst)
mset = re.search(r"cmdstat_set:calls=(\d+),usec=(\d+),usec_per_call=([0-9.]+)", cst)
if mget and int(mget.group(1)) == 40 and int(mget.group(2)) > 0:
    ok("cmdstat_get: exactly 40 calls, usec>0, exact plugin shape")
else:
    bad("cmdstat_get wrong or absent: %r" % (mget.group(0) if mget else cst[:80]))
if mset and int(mset.group(1)) == 25:
    ok("cmdstat_set: exactly 25 calls")
else:
    bad("cmdstat_set wrong or absent")

# ---- CLIENT LIST ----
time.sleep(2)                          # let conn b go idle
cl = call(a, fa, [b"CLIENT", b"LIST"])
clt = cl.decode(errors="replace") if isinstance(cl, bytes) else ""
lines = [l for l in clt.splitlines() if l.strip()]
if len(lines) >= 2:
    ok("CLIENT LIST shows %d clients" % len(lines))
else:
    bad("CLIENT LIST shows %d clients, want >=2: %r" % (len(lines), clt[:80]))
named = [l for l in lines if "name=grafana-probe" in l]
if named:
    ok("SETNAME is visible in CLIENT LIST")
    kv = dict(p.split("=", 1) for p in named[0].split() if "=" in p)
    if int(kv.get("idle", -1)) >= 2 and int(kv.get("age", -1)) >= int(kv.get("idle", 0)):
        ok("idle>=2s on the idle conn, age>=idle")
    else:
        bad("age/idle wrong on idle conn: %r" % named[0])
    if "addr" in kv and kv["addr"].startswith("127.0.0.1:"):
        ok("addr carries ip:port")
    else:
        bad("addr missing/wrong: %r" % kv.get("addr"))
else:
    bad("no client named grafana-probe in CLIENT LIST")

# ---- SLOWLOG (threshold 0: everything logs) ----
n = call(a, fa, [b"SLOWLOG", b"LEN"])
if isinstance(n, int) and n >= 65:
    ok("SLOWLOG LEN=%d (>=65 at threshold 0)" % n)
else:
    bad("SLOWLOG LEN=%r, want >=65" % (n,))
sl = call(a, fa, [b"SLOWLOG", b"GET", b"5"])
if isinstance(sl, list) and len(sl) == 5:
    ok("SLOWLOG GET 5 returns 5 entries")
    e = sl[0]
    if (isinstance(e, list) and len(e) == 6 and isinstance(e[0], int)
            and isinstance(e[1], int) and isinstance(e[2], int)
            and isinstance(e[3], list) and isinstance(e[4], bytes)):
        ok("entry is the 6-field redis shape (id, ts, usec, argv, addr, name)")
    else:
        bad("entry shape wrong: %r" % (e,))
    ids = [e[0] for e in sl]
    if ids == sorted(ids, reverse=True):
        ok("entries are newest-first by id")
    else:
        bad("entries not id-descending: %r" % ids)
else:
    bad("SLOWLOG GET 5 -> %r" % (sl,))
r = call(a, fa, [b"SLOWLOG", b"RESET"])
n2 = call(a, fa, [b"SLOWLOG", b"LEN"])
# at threshold 0 the RESET itself is logged AFTER it executes - redis
# does the same - so the post-RESET log holds at most that one entry
if r == "OK" and n2 <= 1:
    ent = call(a, fa, [b"SLOWLOG", b"GET", b"1"])
    if n2 == 0 or (ent and ent[0][3][0].lower() == b"slowlog"):
        ok("RESET empties the log (bar the RESET itself, as redis)")
    else:
        bad("post-RESET entry is not the RESET: %r" % (ent,))
else:
    bad("RESET -> %r, LEN -> %r" % (r, n2))

a.close(); b.close()
print("PYRESULT %d %d" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
EOF
[ $? -eq 0 ] && ok "surface assertions passed" || bad "surface assertions failed"

kill -9 $P1 2>/dev/null; P1=
sleep 0.3

# ---- default threshold: fast ops must NOT log ----
mkconf "$D/b.conf" ""
start "$D/b.conf" "$D/b.log" || exit 1
P2=$STARTED
python3 - <<'EOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", 17572), 8); s.settimeout(10)
f = s.makefile("rwb")
def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args: out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
def parse():
    h = f.readline(); t, body = h[:1], h[1:].rstrip(b"\r\n")
    if t == b":": return int(body)
    if t in (b"+", b"-"): return body.decode()
    if t == b"$":
        n = int(body)
        return None if n < 0 else f.read(n + 2)[:-2]
    if t == b"*": return [parse() for _ in range(int(body))]
for i in range(30):
    s.sendall(cmd([b"SET", b"k%d" % i, b"v"])); parse()
s.sendall(cmd([b"SLOWLOG", b"LEN"])); n = parse()
print("  %s   default threshold logs nothing fast (LEN=%r)"
      % ("ok " if n == 0 else "FAIL", n))
sys.exit(0 if n == 0 else 1)
EOF
[ $? -eq 0 ] && pass=$((pass+1)) || fail=$((fail+1))

echo "grafanatest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
