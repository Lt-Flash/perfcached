#!/bin/sh
# unknowntest.sh - S165: the commands clients sent that no door implements
# are counted by name and shown, never with their arguments.
#
# One standalone daemon, every door: an unknown RESP command, unsupported
# RESP subcommands (CLIENT, CONFIG) and a known subcommand with the wrong
# arity (NOT a gap), an unknown native JSON method, an unknown binary verb,
# and an unknown command before AUTH (counted, never named).  Then the
# argument that must not leak - it is sent with every unknown RESP command
# and searched for in /stats, /metrics, the page and the journal - the
# NOTICE once per distinct name, 200 random names against the 64-row cap,
# and a stats reset.
# Fail-first: v0.4.0-rc5 has no `unknown_commands` in /stats.
# Usage: test/unknowntest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcunknown.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 1
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = unk-client-secret
resp = unk-resp-password
[listen]
tcp = 127.0.0.1:17791
resp = 127.0.0.1:17792
http = 127.0.0.1:18791
plaintext = loopback
[collection 0]
buckets_log2 = 10
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon never became ready"; tail -5 "$D/n.log"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" LOG="$D/n.log" python3 - <<'PY'
import json, os, socket, struct, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n"); res.flush()
SECRET = "hunter2-S165-secret-arg"
def http(path):
    return urllib.request.urlopen("http://127.0.0.1:18791" + path, timeout=5).read().decode("utf-8", "replace")
def unk(): return json.loads(http("/stats")).get("unknown_commands") or {}
def row(u, name, dialect):
    for r in u.get("rows", []):
        if r.get("name") == name and r.get("dialect") == dialect: return r
    return None

class Resp:
    def __init__(self, auth=True):
        self.s = socket.create_connection(("127.0.0.1", 17792), timeout=5); self.b = b""
        if auth: assert self.cmd("AUTH", "unk-resp-password") == "OK"
    def _line(self):
        while b"\r\n" not in self.b:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.b += d
        ln, self.b = self.b.split(b"\r\n", 1); return ln
    def _read(self):
        ln = self._line(); t, v = ln[:1], ln[1:]
        if t in (b"+", b"-", b":"): return v.decode()
        if t == b"$":
            n = int(v)
            if n < 0: return None
            while len(self.b) < n + 2: self.b += self.s.recv(65536)
            x, self.b = self.b[:n], self.b[n + 2:]; return x.decode("utf-8", "replace")
        if t == b"*": return [self._read() for _ in range(int(v))]
        return v.decode()
    def send(self, *a):
        self.s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x.encode()), x.encode()) for x in a))
    def cmd(self, *a): self.send(*a); return self._read()

def jcall(method):
    s = socket.create_connection(("127.0.0.1", 17791), timeout=5); f = s.makefile("rwb")
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": {"secret": SECRET}}) + "\n").encode()); f.flush()
    r = {}
    for _ in range(64):
        line = f.readline()
        if not line: break
        r = json.loads(line)
        if "id" in r: break
    f.close(); s.close(); return r

def bincall(verb):
    s = socket.create_connection(("127.0.0.1", 17791), timeout=5)
    pay = bytes([verb, 1]) + struct.pack("<H", len(SECRET)) + b"0" + SECRET.encode()
    s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, len(pay), 7) + pay)
    b = b""
    while len(b) < 16:
        d = s.recv(65536)
        if not d: break
        b += d
    s.close(); return b

# 1. before AUTH: counted, never named; a KNOWN command before AUTH is not an unknown
pre = Resp(auth=False)
r1 = pre.cmd("PREAUTHCMD", SECRET)
r2 = pre.cmd("GET", "x")
check(str(r1).startswith("NOAUTH") and str(r2).startswith("NOAUTH"), "before AUTH both answer NOAUTH (%r, %r)" % (r1, r2))
u = unk()
check(u.get("preauth") == 1, "an unknown command before AUTH is counted once, a known one not at all (%r)" % u.get("preauth"))
check(not any("PREAUTH" in (x.get("name") or "") for x in u.get("rows", [])), "and it is not named")

# 2. RESP, authenticated, with a client name
c = Resp()
c.cmd("CLIENT", "SETNAME", "unk-tester")
for _ in range(3): e = c.cmd("XYZZY", SECRET)
check(str(e).startswith("ERR unknown command"), "XYZZY still answers unknown command (%r)" % e)
e = c.cmd("CLIENT", "TRACKING", "on", SECRET)
check(str(e).startswith("ERR unsupported CLIENT subcommand"), "CLIENT TRACKING still answers unsupported (%r)" % e)
c.cmd("CONFIG", "SET", "maxmemory", SECRET)
e = c.cmd("CLIENT", "SETNAME")
check(str(e).startswith("ERR"), "CLIENT SETNAME with no name is refused (%r)" % e)

# 3. native JSON and binary
j = jcall("frobnicate"); jcall("frobnicate")
check(j.get("error", {}).get("code") == -32601, "an unknown JSON method answers -32601 (%r)" % j)
jcall("get")                 # a KNOWN method with bad params is not an unknown
b = bincall(42)
check(len(b) >= 16 and b[0] == 0x9E, "an unknown binary verb gets a binary answer")

u = unk()
x = row(u, "XYZZY", "resp")
check(x is not None and x.get("count") == 3 and x.get("here") == 3, "RESP: XYZZY x3 (%r)" % x)
check(x is not None and x.get("client") == "unk-tester" and (x.get("addr") or "").startswith("127.0.0.1:"),
      "with the client's name and address (%r, %r)" % (x and x.get("client"), x and x.get("addr")))
check(x is not None and x.get("first_s", 0) > 1.7e9 and x.get("last_s", 0) >= x.get("first_s", 0), "first and last seen are unix seconds")
check(row(u, "CLIENT TRACKING", "resp") is not None, "an unsupported subcommand is named with its command: CLIENT TRACKING")
check(row(u, "CONFIG SET", "resp") is not None, "CONFIG SET likewise")
check(row(u, "CLIENT SETNAME", "resp") is None, "a KNOWN subcommand with the wrong arity is not a gap")
check((row(u, "frobnicate", "json") or {}).get("count") == 2, "JSON: frobnicate x2, case kept")
check(row(u, "get", "json") is None, "a known JSON method with bad params is not a gap")
check(row(u, "verb 42", "binary") is not None, "binary: verb 42")
check(u.get("members") == 1 and u.get("reporting") == 1, "standalone: this node alone (%r/%r)" % (u.get("members"), u.get("reporting")))

# 4. the argument never leaks
time.sleep(0.3)
st, mt, pg, lg = http("/stats"), http("/metrics"), http("/"), open(os.environ["LOG"]).read()
check(SECRET not in st and SECRET not in mt and SECRET not in pg and SECRET not in lg,
      "the argument is in none of /stats, /metrics, the page, the journal (%s)" %
      [n for n, t in (("stats", st), ("metrics", mt), ("page", pg), ("log", lg)) if SECRET in t])
def metric(name):
    for ln in mt.splitlines():
        if ln.startswith(name + " "): return int(float(ln.split()[-1]))
    return None
check(metric('perfcached_commands_unknown_total{dialect="resp"}') == 5 and
      metric('perfcached_commands_unknown_total{dialect="json"}') == 2 and
      metric('perfcached_commands_unknown_total{dialect="binary"}') == 1 and
      metric("perfcached_commands_unknown_preauth_total") == 1,
      "/metrics: resp 5, json 2, binary 1, preauth 1 (%r)" % [ln for ln in mt.splitlines() if "unknown" in ln and not ln.startswith("#")])
firsts = lambda: open(os.environ["LOG"]).read().count("first sighting on this node")
check(firsts() == 5, "one NOTICE per distinct name so far: 5 (%r)" % firsts())
c.cmd("XYZZY", SECRET)
time.sleep(0.2)
check(firsts() == 5, "a repeat logs nothing (%r)" % firsts())
check("unknown command 'XYZZY' (resp) from 127.0.0.1:" in lg and "[unk-tester]" in lg, "the NOTICE names the command, the door, the address and the client")

# 5. 200 random names against the cap
for i in range(200): c.send("RND%03d" % i, SECRET)
for i in range(200): c._read()
u = unk()
check(len(u.get("rows", [])) == 64, "the table stops at 64 rows (%r)" % len(u.get("rows", [])))
check(u.get("other") == 200 - (64 - 5), "the rest are counted in other: %r (%r)" % (200 - 59, u.get("other")))
check(row(u, "XYZZY", "resp") is not None, "the flood did not push the real gap off the table")
time.sleep(0.3)
check(firsts() == 64, "one NOTICE per STORED name, not per send: 64 (%r)" % firsts())

# 6. the page renders it
check("function unkcard(" in pg and "cmdcards(s)+unkcard(s)+" in pg, "the page carries the unknown-commands card beside the slow log")

# 7. a stats reset empties it
check(c.cmd("CONFIG", "RESETSTAT") == "OK", "CONFIG RESETSTAT")
u = unk()
check(u.get("rows") == [] and u.get("other") == 0 and u.get("preauth") == 0, "a stats reset empties the table (%r rows)" % len(u.get("rows", [])))
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "unknowntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
