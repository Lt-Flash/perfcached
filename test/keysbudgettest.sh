#!/bin/sh
# keysbudgettest.sh - S292: a cooperative KEYS walk steps by a TIME
# budget per worker turn, not by a fixed bucket count.
#
# At 1,024 buckets a turn a walk's turn count scaled with the table's
# size: 1,000 keys in 131,072 buckets took 128 turns, and under load each
# turn is as long as everything else the worker serves in it - a 1-match
# KEYS waited 64-112 ms at p99 where Redis took 10 (keyscanbench, DESIGN
# 12hl).  Turns are counted (/stats resp.keys_walks, keys_turns), never
# timed:
#   1  sparse: 1,000 keys in 131,072 buckets - every walk ends in at most
#      32 turns (about 6 at the default 250 us on 222; it was 128), and
#      returns every key
#   2  dense: 100,000 keys - a full walk still YIELDS (more than one
#      turn), so the stall bound the cooperative walk exists for holds
#   3  the same sparse walk on a daemon with keys_turn_us = 0 (the floor
#      only - 0.4.0's walk) takes 128 turns: the budget is what does it,
#      and this is the fail-first, run every time
# Usage: test/keysbudgettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pckb.XXXXXX)
PID= PID0=
trap '[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; [ -n "$PID0" ] && kill -9 "$PID0" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
if ss -ltn 2>/dev/null | grep -qE ":1757[3-6][[:space:]]"; then
	echo "keysbudgettest: a port in 17573-17576 is already bound" >&2; exit 1
fi
conf() { # conf <name> <native port> <resp port> [keys_turn_us line]
	cat > "$D/$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
$4
[memory]
arena_mb = 128
[secrets]
client = kb-client-secret
cluster = kb-cluster-secret
[listen]
tcp = 127.0.0.1:$2
resp = 127.0.0.1:$3
plaintext = loopback
[collection 0]
buckets_log2 = 17
EOF
	chmod 600 "$D/$1.conf"
}
up() { # up <name>
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "keysbudgettest: daemon $1 did not start"; cat "$D/$1.log"; exit 1
}
conf n 17573 17574 ""                       # the default budget
conf f 17575 17576 "keys_turn_us = 0"       # the floor only
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
"$BIN" -f "$D/f.conf" > "$D/f.log" 2>&1 & PID0=$!
up n; up f

python3 - <<'EOF'
import json, socket, sys

RESP, NATIVE = 17574, 17573
FRESP, FNATIVE = 17576, 17575
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
def native(method, port=NATIVE, **params):
    s = socket.create_connection(("127.0.0.1", port), timeout=10); f = s.makefile("rwb")
    f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode() + b"\n"); f.flush()
    r = json.loads(f.readline()); s.close()
    return r.get("result", r)
def figures(port=NATIVE):
    st = native("stats", port)
    r = st.get("resp") or {}
    b = [c.get("buckets") for c in st.get("collections", []) if c.get("name") == "0"]
    return r.get("keys_walks"), r.get("keys_turns"), (b[0] if b else None)

class Conn:
    def __init__(self, port=RESP):
        self.s = socket.create_connection(("127.0.0.1", port), 10)
        self.s.settimeout(60); self.b = bytearray(); self.p = 0
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
    def keys(self, pat):
        del self.b[:self.p]; self.p = 0
        self.s.sendall(cmd([b"KEYS", pat]))
        l = self.line()
        if not l.startswith(b"*"):
            return -1
        n = int(l[1:])
        for _ in range(n):
            ln = int(self.line()[1:]) + 2
            while len(self.b) - self.p < ln:
                self.fill()
            self.p += ln
        return n
    def fill_keys(self, lo, hi):
        parts = [cmd([b"SET", b"k:%09d" % i, b"v"]) for i in range(lo, hi)]
        parts.append(cmd([b"DBSIZE"]))
        self.s.sendall(b"".join(parts))
        want = b":%d\r\n" % hi
        buf = b""
        while want not in buf:
            d = self.s.recv(1 << 20)
            if not d:
                sys.exit("keysbudgettest: the fill connection died")
            buf = buf[-64:] + d

c = Conn()
w0, t0, nb = figures()
if w0 is None or t0 is None:
    bad("no resp.keys_walks / keys_turns in /stats")
    print("keysbudgettest: %d passed, %d failed" % (pass_n, fail_n)); sys.exit(1)

print("--- 1: sparse - 1,000 keys in 131,072 buckets")
c.fill_keys(0, 1000)
w0, t0, nb = figures()
counts = [c.keys(b"*") for _ in range(5)] + [c.keys(b"zz*")]
w1, t1, nb1 = figures()
if nb != 131072 or nb1 != 131072:
    bad("the table is not 131,072 buckets around the walks (%s -> %s) - this proves nothing" % (nb, nb1))
walks, turns = w1 - w0, t1 - t0
print("    6 walks over %s buckets: %d walks, %d turns (%.1f a walk)" % (nb1, walks, turns, turns / max(walks, 1)))
if counts == [1000] * 5 + [0]:
    ok("every walk returned every key (and none for no match)")
else:
    bad("wrong replies: %s" % counts)
if walks == 6:
    ok("six walks counted")
else:
    bad("walks counted %d, want 6" % walks)
if walks and turns <= 32 * walks:
    ok("at most 32 turns a walk (%d turns for %d walks; 0.4.0's 1,024 a turn: 128 each)" % (turns, walks))
else:
    bad("%d turns for %d walks - the walk still steps by bucket count" % (turns, walks))

print("--- 2: dense - 100,000 keys: a full walk still yields")
c.fill_keys(1000, 100000)
w0, t0, nb = figures()
n = c.keys(b"*")
w1, t1, nb1 = figures()
print("    one walk of %d keys over %s buckets: %d turn(s)" % (n, nb1, t1 - t0))
if n == 100000:
    ok("the walk returned all 100,000 keys")
else:
    bad("the walk returned %d keys, want 100000" % n)
if w1 - w0 == 1 and t1 - t0 >= 2:
    ok("it yielded: %d turns, not one long stall" % (t1 - t0))
else:
    bad("walks %d, turns %d - a dense walk must not finish in one turn" % (w1 - w0, t1 - t0))

print("--- 3: the same sparse walk with keys_turn_us = 0 (0.4.0's floor)")
f = Conn(FRESP)
f.fill_keys(0, 1000)
w0, t0, nb = figures(FNATIVE)
n = f.keys(b"*")
w1, t1, nb1 = figures(FNATIVE)
print("    one walk of %d keys over %s buckets: %d turn(s)" % (n, nb1, t1 - t0))
if n == 1000 and w1 - w0 == 1 and t1 - t0 == 128:
    ok("128 turns - one per 1,024 buckets, so the budget is what shortened part 1")
else:
    bad("keys %d, walks %d, turns %d - want 1000, 1, 128" % (n, w1 - w0, t1 - t0))

print("keysbudgettest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
EOF
