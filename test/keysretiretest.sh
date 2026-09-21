#!/bin/sh
# keysretiretest.sh - S150 step B: a KEYS walk resumes across worker
# turns holding a table pointer, and a resize swaps the table under it.
#
# Two things must hold.  The walk must END on the swap - what was
# emitted stands - rather than read on from a table the registry no
# longer publishes (step C reuses such a table).  And the old table must
# be RETIRED only once every thread that could hold a pointer into it
# has parked: /stats shows it pending, then cleared.  Three connections
# keep no-match KEYS walks (full-length, 4-byte reply) running through
# the whole resize, so the swap lands under a walk in flight.
#
# Fail-first: before B there is no `retire` object in /stats and no walk
# ends on a swap - every figure below reads MISSING.
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "keysretiretest: $CLI not built - SKIPPED, and a skip is not a pass"; exit 0; }
D=$(mktemp -d /var/tmp/pckr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
if ss -ltn 2>/dev/null | grep -qE ":1759[12][[:space:]]"; then
	echo "keysretiretest: port 17591/17592 already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = 256
[secrets]
client = kr-client-secret
cluster = kr-cluster-secret
enable = kr-enable
[listen]
tcp = 127.0.0.1:17591
resp = 127.0.0.1:17592
plaintext = loopback
[collection 0]
buckets_log2 = 18
EOF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "daemon did not start"; cat "$D/n.log"; exit 1; }

python3 - "$CLI" <<'EOF'
import json, socket, subprocess, sys, threading, time

CLI = sys.argv[1]
RESP, NATIVE = 17592, 17591
NKEYS = 100000
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def conn():
    s = socket.create_connection(("127.0.0.1", RESP), 8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(30)
    return s
def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
def native(method, **params):
    s = socket.create_connection(("127.0.0.1", NATIVE), timeout=10); f = s.makefile("rwb")
    f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode() + b"\n"); f.flush()
    r = json.loads(f.readline()); s.close()
    return r.get("result", r)
def retire():
    return native("stats").get("retire") or {}

class Resp:
    """a multi-bulk reader that parses in place - 100k items must not
    cost 100k buffer copies"""
    def __init__(self, s):
        self.s = s; self.b = bytearray(); self.p = 0
    def fill(self):
        d = self.s.recv(1 << 20)
        if not d:
            raise EOFError("connection closed")
        self.b += d
    def line(self):
        while True:
            i = self.b.find(b"\r\n", self.p)
            if i >= 0:
                l = bytes(self.b[self.p:i]); self.p = i + 2; return l
            self.fill()
    def keys(self):
        del self.b[:self.p]; self.p = 0
        l = self.line()
        if not l.startswith(b"*"):
            return ("bad", l)
        n = int(l[1:])
        for _ in range(n):
            l = self.line()
            if not l.startswith(b"$"):
                return ("bad", l)
            ln = int(l[1:]) + 2
            while len(self.b) - self.p < ln:
                self.fill()
            self.p += ln
        return ("ok", n)

# ---- fill ----
s = conn(); parts = []; plen = 0; t_fill = time.time()
for i in range(NKEYS):
    c = cmd([b"SET", b"k:%09d" % i, b"v"]); parts.append(c); plen += len(c)
    if plen > 1 << 20:
        s.sendall(b"".join(parts)); parts = []; plen = 0
        s.setblocking(False)
        try:
            while s.recv(1 << 20): pass
        except BlockingIOError:
            pass
        s.setblocking(True); s.settimeout(30)
s.sendall(b"".join(parts) + cmd([b"DBSIZE"]))
buf = b""
while b":%d\r\n" % NKEYS not in buf:
    d = s.recv(1 << 20)
    if not d: sys.exit("fill connection died")
    buf = (buf + d)[-64:]
ok("filled %d keys in a 2^18 table" % NKEYS)
s.close()

r0 = retire()
if r0 and r0.get("pending") == 0 and r0.get("cleared") == 0 and r0.get("keys_ended_on_swap") == 0 and r0.get("lines", 0) >= 3:
    ok("before: nothing retired, no walk ended on a swap, %d quiescence lines" % r0["lines"])
else:
    bad("before: retire figures %r (MISSING = the tree before B)" % (r0,))

# ---- walkers: full-length no-match walks, back to back ----
stop = False
stats = {"walks": 0, "bad": 0, "err": ""}
lock = threading.Lock()
def walker():
    try:
        c = conn(); rd = Resp(c)
        while not stop:
            c.sendall(cmd([b"KEYS", b"zzz*"]))
            st, v = rd.keys()
            with lock:
                if st == "ok" and v == 0:
                    stats["walks"] += 1
                else:
                    stats["bad"] += 1; stats["err"] = repr(v)[:60]
        c.close()
    except Exception as e:
        with lock:
            stats["bad"] += 1; stats["err"] = repr(e)[:80]
ths = [threading.Thread(target=walker) for _ in range(3)]
for t in ths: t.start()
time.sleep(0.5)

# ---- the resize, down to 2^16, while they walk ----
out = subprocess.run([CLI, "-q", "-h", "127.0.0.1", "-p", str(NATIVE), "-E", "kr-enable",
                      "resize", "0", "16"], capture_output=True, text=True).stdout
if "resizing" in out:
    ok("resize 0 -> 2^16 accepted under three running walks")
else:
    bad("resize refused: %s" % out.strip()[:80])

t0 = time.time(); r = {}; pend_seen = 0
while time.time() - t0 < 40:
    r = retire()
    if r.get("pending", 0) > 0:
        pend_seen += 1
    if r.get("cleared", 0) >= 1:
        break
    time.sleep(0.25)
took = time.time() - t0
stop = True
for t in ths: t.join()

if r.get("cleared") == 1 and r.get("pending") == 0:
    ok("the old table was retired and cleared %.1fs after the resize verb (pending seen %d polls)" % (took, pend_seen))
else:
    bad("retire after %.1fs: %r" % (took, r))
if stats["bad"] == 0 and stats["walks"] > 0:
    ok("%d walks through the resize, every reply well-formed" % stats["walks"])
else:
    bad("%d walks, %d bad replies: %s" % (stats["walks"], stats["bad"], stats["err"]))
k = r.get("keys_ended_on_swap", 0)
if k >= 1:
    ok("%d walk(s) ended on the swap instead of reading the old table on" % k)
else:
    bad("no walk ended on the swap (keys_ended_on_swap=%r) - the handle is not checked" % k)

# ---- after: the table is 2^16 and every key is still there ----
cols = native("collections")
cols = cols.get("collections", cols) if isinstance(cols, dict) else cols
e = next((c for c in cols if str(c.get("name")) == "0"), None) if isinstance(cols, list) else None
if e is not None and (e.get("buckets_log2") == 16 or e.get("buckets") == 1 << 16):
    ok("collection 0 is at 2^16")
else:
    bad("collection 0 after the resize: %r" % (e,))
c = conn(); rd = Resp(c)
c.sendall(cmd([b"KEYS", b"*"]))
st, n = rd.keys()
if st == "ok" and n == NKEYS:
    ok("a fresh KEYS * after the resize returns all %d keys" % n)
else:
    bad("KEYS * after the resize: %s %r" % (st, n))
c.close()
print("keysretiretest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
EOF
rc=$?
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=
exit $rc
