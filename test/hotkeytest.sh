#!/bin/sh
# hotkeytest.sh - S397 (DESIGN 12ng): hot keys, always on.  One request in
# 64 is sampled into a space-saving sketch per collection, counted where it
# enters the node's data-verb cores - so every door is one hook - and
# shown in /stats collections[].hot_keys {rate, sampled, sampled_total,
# busy, top[{key, key_len, cut, est, err, share_pm}]}.  Cases:
#   A. CMD door: a key at half the traffic and one at a quarter are the
#      top two, at plausible shares; the sampled count is about 1 in 64
#   B. the fixed-layout door: a 100-byte key is kept as its first 64 bytes,
#      cut = true, key_len = 100
#   C. the RESP door: GET, each key of an MGET, and a hash command (HSET's
#      key, argv[1]) are counted
#   D. the counts halve on the tick: a second daemon at
#      PERFCACHED_TEST_HOT_HALF_S=2 (60 s in a fleet); the first holds its
#      counts for the suite (3600) so A-C read them whole
#   E. reset-stats empties the sketch
# The sample is random (a per-thread xorshift, so a client cycling through
# 64 keys is not aliased onto one), so the bounds are wide: a binomial at
# these counts sits well inside them.
# Fail-first: before S397 /stats has no hot_keys.
# Usage: test/hotkeytest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pchot.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkconf() { # mkconf <file> <tcp> <resp> <http>
	cat > "$1" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = hk-client-secret
cluster = hk-cluster-secret
[listen]
tcp = 127.0.0.1:$2
resp = 127.0.0.1:$3
http = 127.0.0.1:$4
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
	chmod 600 "$1"
}
start() { # start <conf> <log> <half-life s> -> pid in $STARTED
	PERFCACHED_TEST_HOT_HALF_S=$3 "$BIN" -f "$1" > "$2" 2>&1 &
	STARTED=$!
	i=0; while [ $i -lt 80 ] && ! grep -q "perfcached ready" "$2"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$2" || { echo "daemon did not start"; cat "$2"; return 1; }
}
# A holds its counts for the length of the suite (A-C, E); B halves every 2 s (D)
mkconf "$D/a.conf" 16931 16932 17931; start "$D/a.conf" "$D/a.log" 3600 || exit 1; P1=$STARTED
mkconf "$D/b.conf" 16934 16935 17938; start "$D/b.conf" "$D/b.log" 2 || exit 1; P2=$STARTED
# the line is written by the first maintenance tick, about a second after ready
i=0; while [ $i -lt 50 ] && ! grep -q 'PERFCACHED_TEST_HOT_HALF_S=2 ' "$D/b.log"; do sleep 0.1; i=$((i+1)); done
grep -q 'PERFCACHED_TEST_HOT_HALF_S=2 ' "$D/b.log" && ok "the test half-life is named in the log" || bad "no test half-life line"

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, random, socket, struct, time
import pcnative
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n")

class Cmd:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = pcnative.wrap(self.s); self.rid = 0
    def req(self, method, params):
        self.rid += 1
        self.f.write((json.dumps({"jsonrpc": "2.0", "id": self.rid, "method": method,
                                  "params": params}) + "\n").encode()); self.f.flush()
        return json.loads(self.f.readline())
A = Cmd(16931); B = Cmd(16934)
req = A.req
def hot(c=None):
    st = (c or A).req("stats", {"col": "0"})["result"]
    for c in st["collections"]:
        if c["name"] == "0":
            return c.get("hot_keys")
    return None

h = hot()
check(h is not None and h.get("rate") == 64 and h.get("sampled") == 0 and h.get("top") == [],
      "a fresh collection: hot_keys with rate 64, nothing sampled, no rows (%r)" % (h,))

# ---- A. the CMD door: 'hot' at 1/2, 'warm' at 1/4, the rest unique ------------------
N = 12800
req("set", {"col": "0", "key": "hot", "value": "v"})
req("set", {"col": "0", "key": "warm", "value": "v"})
rnd = random.Random(397)
for i in range(N):
    r = rnd.random()
    k = "hot" if r < 0.5 else "warm" if r < 0.75 else "u%d" % i
    req("get", {"col": "0", "key": k})
h = hot()
top = h["top"] if h else []
names = [t["key"] for t in top]
# N + 2 requests over 64: ~200 expected; a binomial's sd is ~14
check(h and 120 <= h["sampled"] <= 300 and h["sampled_total"] == h["sampled"],
      "A. about 1 in 64 sampled: %s of %d requests" % (h and h["sampled"], N + 2))
check(names[:2] == ["hot", "warm"], "A. the top two are hot, then warm: %r" % names[:4])
sh = {t["key"]: t["share_pm"] for t in top}
check(380 <= sh.get("hot", 0) <= 620 and 150 <= sh.get("warm", 0) <= 350,
      "A. shares about 500 and 250 per mille: hot %s, warm %s" % (sh.get("hot"), sh.get("warm")))
check(all(t["est"] - t["err"] >= 2 for t in top) and not any(n.startswith("u") for n in names),
      "A. no row is listed unless surely seen twice (est - err >= 2): none of the unique keys")
check(all(t["est"] >= t["err"] >= 0 and t["cut"] is False and t["key_len"] == len(t["key"]) for t in top),
      "A. every row: est >= err >= 0, short keys whole")

# ---- B. the fixed-layout door: a 100-byte key ---------------------------------------
MAGIC = 0x9E
def freq(payload, r):
    return struct.pack("<BBBBIQ", MAGIC, 1, 1, 0, len(payload), r) + payload
def vh(verb, col, key, extra=b"", val=b""):
    return bytes([verb, len(col)]) + struct.pack("<H", len(key)) + extra + col + key + val
def rexact(so, n):
    b = b""
    while len(b) < n:
        d = so.recv(n - len(b))
        if not d: raise EOFError
        b += d
    return b
def bmsg(so):
    hd = rexact(so, 16)
    magic, ver, typ, flags, ln, r = struct.unpack("<BBBBIQ", hd)
    return rexact(so, ln)
LK = b"L" * 100
b = socket.create_connection(("127.0.0.1", 16931), timeout=10)
r = 1
b.sendall(freq(vh(3, b"0", LK, struct.pack("<q", 0), b"v"), r)); bmsg(b)
for i in range(6400):
    r += 1
    b.sendall(freq(vh(2, b"0", LK), r)); bmsg(b)
b.close()
h = hot()
row = [t for t in h["top"] if t["key"].startswith("LLLL")]
check(len(row) == 1 and row[0]["key"] == "L" * 64 and row[0]["key_len"] == 100 and row[0]["cut"] is True,
      "B. a 100-byte key through the fixed-layout door: its first 64 bytes, key_len 100, cut (%r)" % (row[:1],))

# ---- C. the RESP door: GET, MGET's keys, HSET ---------------------------------------
rs = socket.create_connection(("127.0.0.1", 16932), timeout=10)
rf = rs.makefile("rwb")
def resp(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        x = x.encode() if isinstance(x, str) else x
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    rf.write(out); rf.flush()
    line = rf.readline()
    if line[:1] == b"$" and int(line[1:]) >= 0: rf.read(int(line[1:]) + 2)
    elif line[:1] == b"*":
        for _ in range(int(line[1:])):
            l2 = rf.readline()
            if l2[:1] == b"$" and int(l2[1:]) >= 0: rf.read(int(l2[1:]) + 2)
    return line
resp("SET", "rget", "v")
for i in range(3200): resp("GET", "rget")
for i in range(1600): resp("MGET", "rmget", "rmget", "x%d" % i)
for i in range(3200): resp("HSET", "rhash", "f%d" % (i % 8), "v")
rs.close()
h = hot()
names = [t["key"] for t in h["top"]]
check("rget" in names and "rmget" in names and "rhash" in names,
      "C. RESP GET, each key of an MGET, and HSET's key are counted: %r" % names[:8])

# ---- D. the counts halve on the tick (daemon B, 2 s) --------------------------------
B.req("set", {"col": "0", "key": "hot", "value": "v"})
for i in range(6400): B.req("get", {"col": "0", "key": "hot"})
h = hot(B); s0 = h["sampled"]; e0 = {t["key"]: t["est"] for t in h["top"]}
time.sleep(4.5)
h = hot(B)
check(s0 >= 40 and h["sampled"] <= s0 // 2 and h["sampled_total"] >= s0,
      "D. after two half-lives the sampled count fell from %d to %d; the total kept %d" % (s0, h["sampled"], h["sampled_total"]))
e1 = {t["key"]: t["est"] for t in h["top"]}
check(e1.get("hot", 0) <= e0.get("hot", 0) // 2 and e1.get("hot", 0) >= 2,
      "D. hot's count halved with it: %s -> %s" % (e0.get("hot"), e1.get("hot")))

# ---- E. reset-stats -----------------------------------------------------------------
req("reset_stats", {})
h = hot()
check(h["sampled"] == 0 and h["sampled_total"] == 0 and h["top"] == [],
      "E. reset-stats empties the sketch (%r)" % ({k: h[k] for k in ("sampled", "sampled_total")},))
A.f.close(); B.f.close()
PY
while read -r st name; do
	[ "$st" = P ] && ok "$name" || bad "$name"
done < "$RES"
[ -s "$RES" ] || bad "the driver wrote no result"
kill $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null; P1= P2=
echo "hotkeytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
