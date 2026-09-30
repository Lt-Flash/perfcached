#!/bin/sh
# setnxtest.sh - S279: SET key value [EX n] NX|XX on the RESP door.
#
# A production Redis's clients send `SET k v EX n NX` - 51% of their SETs,
# measured 2026-09-30 - and perfcached refused every one ("unsupported
# SET option").  It is the lock idiom: take it if nobody holds it, for n
# seconds.  A lock is only a lock if two clients cannot both be granted
# it, so in a cluster every NX/XX of a key is decided by ONE node, the
# slot's owner; eager and spread otherwise let each node decide alone.
#
#   A. one unclustered node: NX on absent -> OK, on present -> nil; XX on
#      absent -> nil, on present -> OK; EX rides along (TTL); an expired
#      key is absent to NX; NX with XX is a syntax error; KEEPTTL and GET
#      are still refused; 16 clients racing NX on one key, 30 rounds:
#      exactly one OK per round
#   B. three EAGER nodes: 12 clients spread over all three doors race
#      `SET k v EX 30 NX`, 40 rounds: exactly one OK per round, and every
#      node then answers the winner's value
#   C. three SHARD nodes: NX and XX through a node that does not own the
#      key are forwarded - OK, nil, OK, nil as in A - and the owner holds
#      the value
#   B and C also drive the NATIVE doors through every node (S279b):
#      JSON-RPC setnx/setxx and binary SETNX/SETXX frames - a non-owner
#      parks the request and completes it from the owner's answer, the
#      path only a forward exercises
#   D. a PROXY cluster has no node that decides a key: NX is refused,
#      said, and nothing is stored
# The standalone edition refuses [cluster], so there B and C are SKIPPED,
# loudly.
# Fail-first: rc58 refuses NX ("unsupported SET option") - A, B and C
# fail.  A build that decides NX on the receiving node in eager fails B's
# one-winner count (the mutation this suite was checked against).
# Usage: test/setnxtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsnx.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skp() { skip=$((skip+1)); echo "  SKIP $1"; }
for p in 18901 18902 18903 18911 18912 18913 18921 18922 18923; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "setnxtest: port $p busy" >&2; exit 1; }
done
SA=; "$BIN" -V 2>/dev/null | grep -q standalone && SA=1

conf() { # conf <n> <mode or empty>
	mkdir -p "$D/s$1"
	CL=
	[ -n "$2" ] && CL="[cluster]
multicast = 239.255.78.13:18905
advertise = 127.0.28.$1
mode = $2
collections = 0"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = snx-client-secret
cluster = snx-cluster-secret
[listen]
tcp = 127.0.0.1:1890$1
resp = 127.0.0.1:1891$1
http = 127.0.0.1:1892$1
plaintext = loopback
$CL
[collection 0]
buckets_log2 = 12
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
stopall() { for f in "$D"/*.pid; do [ -f "$f" ] && { kill "$(cat "$f")" 2>/dev/null; rm -f "$f"; }; done; sleep 1; rm -rf "$D"/s*; }
st() { curl -s -m 3 "http://127.0.0.1:1892$1/stats" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null; }
formed() {
	i=0; while [ $i -lt 200 ]; do
		[ "$(st 1)$(st 2)$(st 3)" = "ready 2ready 2ready 2" ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}
# drive <part> <ports...>: the Python driver, results as ok/FAIL lines
drive() {
	part=$1; shift
	timeout 240 python3 - "$part" "$@" <<'PY' > "$D/py.$part" 2>&1
import socket, sys, threading, time, json, urllib.request
part, ports = sys.argv[1], [int(p) for p in sys.argv[2:]]
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
class R:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10); self.buf = b""
    def cmd(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + x.encode() + b"\r\n" for x in a))
        return self.read()
    def _line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        l, self.buf = self.buf.split(b"\r\n", 1); return l
    def read(self):
        l = self._line(); t, b = l[:1], l[1:]
        if t == b"+": return b.decode()
        if t == b"-": return "ERR:" + b.decode()
        if t == b":": return int(b)
        if t == b"$":
            n = int(b)
            if n < 0: return None
            while len(self.buf) < n + 2:
                d = self.s.recv(65536)
                if not d: raise EOFError
                self.buf += d
            v, self.buf = self.buf[:n], self.buf[n + 2:]; return v.decode(errors="replace")
        return "<?>"

class N:
    """the native doors on the tcp port: JSON-RPC lines and binary frames"""
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10); self.f = self.s.makefile("rb"); self.id = 0
    def rpc(self, method, **params):
        self.id += 1
        self.s.sendall((json.dumps({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params}) + "\n").encode())
        return json.loads(self.f.readline())
    def bin(self, verb, col, key, val, ttl=0):
        import struct
        self.id += 1
        pl = struct.pack("<BBHq", verb, len(col), len(key), ttl) + col.encode() + key.encode() + val.encode()
        self.s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, len(pl), self.id) + pl)
        hdr = self.f.read(16)
        flags, n = hdr[3], struct.unpack("<I", hdr[4:8])[0]
        body = self.f.read(n)
        return ("ERR", body.decode(errors="replace")) if flags & 1 else body

def settled(port, key, want, secs=3.0):
    """eager replicates by push, batched: a read through another node
    may precede it - wait for the value rather than read once"""
    t = time.time() + secs; c = R(port)
    while True:
        v = c.cmd("GET", key)
        if v == want or time.time() > t: return v == want
        time.sleep(0.05)

def race(key, nclients, rounds, ttl="30"):
    """nclients over all ports race SET NX on @key; returns (bad rounds, per-round winners, last winner token)"""
    conns = [R(ports[i % len(ports)]) for i in range(nclients)]
    admin = R(ports[0])
    badr, wins, last = [], [], None
    for rnd in range(rounds):
        k = "%s:%d" % (key, rnd)
        bar = threading.Barrier(nclients)
        out = [None] * nclients
        def go(i):
            bar.wait()
            out[i] = conns[i].cmd("SET", k, "tok-%d-%d" % (rnd, i), "EX", ttl, "NX")
        th = [threading.Thread(target=go, args=(i,)) for i in range(nclients)]
        for t in th: t.start()
        for t in th: t.join()
        n = sum(1 for x in out if x == "OK")
        other = [x for x in out if x not in ("OK", None)]
        wins.append(n)
        if n != 1 or other:
            badr.append((rnd, n, other[:2]))
        else:
            last = (k, "tok-%d-%d" % (rnd, out.index("OK")))
    return badr, wins, last

if part == "D":
    c = R(ports[0])
    r = c.cmd("SET", "d", "1", "NX")
    ok(isinstance(r, str) and "needs a shard or eager" in r and c.cmd("GET", "d") is None, "proxy: NX refused, nothing stored (%r)" % r)
    n = N(ports[0] - 10)
    r = n.rpc("setnx", col="0", key="d", value="1")
    b = n.bin(15, "0", "d", "1")
    ok("error" in r and "needs a shard or eager" in r["error"].get("message", "") and isinstance(b, tuple) and "needs a shard or eager" in b[1], "proxy: native setnx refused on both dialects")
elif part == "A":
    c = R(ports[0])
    ok(c.cmd("SET", "a", "1", "NX") == "OK", "NX on an absent key: OK")
    ok(c.cmd("SET", "a", "2", "NX") is None and c.cmd("GET", "a") == "1", "NX on a present key: nil, value unchanged")
    ok(c.cmd("SET", "b", "1", "XX") is None and c.cmd("GET", "b") is None, "XX on an absent key: nil, nothing stored")
    ok(c.cmd("SET", "a", "3", "XX") == "OK" and c.cmd("GET", "a") == "3", "XX on a present key: OK, value replaced")
    r = c.cmd("SET", "l", "t", "EX", "30", "NX"); t = c.cmd("TTL", "l")
    ok(r == "OK" and isinstance(t, int) and 25 <= t <= 30, "SET EX 30 NX: OK with TTL %r" % t)
    ok(c.cmd("SET", "l", "u", "NX", "EX", "30") is None, "option order does not matter: NX before EX, key held -> nil")
    ok(c.cmd("SET", "e", "t", "EX", "1", "NX") == "OK", "a 1 s lock")
    time.sleep(2.2)
    ok(c.cmd("SET", "e", "t2", "EX", "5", "NX") == "OK" and c.cmd("GET", "e") == "t2", "an expired key is absent to NX")
    r = c.cmd("SET", "x", "1", "NX", "XX")
    ok(isinstance(r, str) and r.startswith("ERR") and "syntax" in r, "NX with XX: %r" % r)
    r1, r2 = c.cmd("SET", "x", "1", "KEEPTTL"), c.cmd("SET", "x", "1", "GET")
    ok(all(isinstance(r, str) and "unsupported SET option" in r for r in (r1, r2)), "KEEPTTL and GET still refused")
    ok(c.cmd("DEL", "a") == 1 and c.cmd("SET", "a", "4", "NX") == "OK", "released by DEL, NX takes it again")
    badr, wins, _ = race("ra", 16, 30)
    ok(not badr, "16 clients racing NX on one node, 30 rounds: one OK each (bad rounds %r)" % badr[:3])
else:
    badr, wins, last = race("rb", 12, 40)
    ok(not badr, "%s: 12 clients over %d nodes racing SET EX 30 NX, 40 rounds: exactly one OK each (bad rounds %r)" % (part, len(ports), badr[:3]))
    if last:
        time.sleep(1.5)
        vals = [R(p).cmd("GET", last[0]) for p in ports]
        ok(all(v == last[1] for v in vals), "%s: every node answers the winner's value (%r)" % (part, vals))
    # the deciding node is the owner: a client on any other door is
    # forwarded, so the forward path MUST have run - an eager build that
    # decided locally would serve none
    st = [json.load(urllib.request.urlopen("http://127.0.0.1:1892%d/stats" % (i + 1), timeout=5))["cluster"] for i in range(len(ports))]
    served = sum(int(s.get("fwd_served", 0)) for s in st)
    ok(served >= 40, "%s: the owner decided - %d forwards served across the fleet (want >= 40)" % (part, served))
    # S279b: the native doors, through every node
    tcp = [p - 10 for p in ports]
    for i, p in enumerate(tcp):
        n = N(p); k = "nj%d" % i
        seq = [n.rpc("setnx", col="0", key=k, value="1", ttl=30).get("result"),
               n.rpc("setnx", col="0", key=k, value="2").get("result"),
               n.rpc("setxx", col="0", key=k, value="3").get("result"),
               n.rpc("setxx", col="0", key=k + "-no", value="1").get("result")]
        want = [{"stored": True}, {"stored": False}, {"stored": True}, {"stored": False}]
        ok(seq == want and settled(ports[i], k, "3"), "%s json-rpc via node %d: setnx/setnx/setxx/setxx -> %r" % (part, i + 1, seq))
        k = "nb%d" % i
        seq = [n.bin(15, "0", k, "1", 30), n.bin(15, "0", k, "2"), n.bin(16, "0", k, "3"), n.bin(16, "0", k + "-no", "1")]
        ok(seq == [b"\x01", b"\x00", b"\x01", b"\x00"] and settled(ports[i], k, "3"), "%s binary via node %d: SETNX/SETNX/SETXX/SETXX -> %r" % (part, i + 1, seq))
    if part == "C":
        # the forwarded path explicitly: through each node, one key, the
        # same sequence as A - whichever node owns it, two of the three
        # doors forward
        for i, p in enumerate(ports):
            c = R(p); k = "fw%d" % i
            seq = [c.cmd("SET", k, "1", "EX", "30", "NX"), c.cmd("SET", k, "2", "NX"),
                   c.cmd("SET", k, "3", "XX"), c.cmd("SET", k + "-no", "1", "XX")]
            ok(seq == ["OK", None, "OK", None] and c.cmd("GET", k) == "3", "C via node %d: NX/NX/XX/XX -> %r, value %r" % (i + 1, seq, c.cmd("GET", k)))
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
	grep -E "^  (ok|FAIL|info) " "$D/py.$part"
	pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.$part" | awk '{print $2}')
	[ -n "$pf" ] || { bad "$part: the driver did not finish: $(tail -3 "$D/py.$part" | tr '\n' ' ' | cut -c1-300)"; pf=0; }
	pass=$((pass + $(grep -c "^  ok " "$D/py.$part"))); fail=$((fail + pf))
}

echo "--- A. one unclustered node"
conf 1 ""
if start 1; then drive A 18911; else bad "A: node did not start"; fi
stopall

for m in eager shard; do
	[ $m = eager ] && part=B || part=C
	echo "--- $part. three $m nodes"
	if [ -n "$SA" ]; then skp "$part: the standalone edition refuses a [cluster] section"; continue; fi
	for n in 1 2 3; do conf $n $m; done
	up=1; for n in 1 2 3; do start $n || up=; done
	if [ -n "$up" ] && formed; then
		drive $part 18911 18912 18913
	else
		bad "$part: the $m fleet did not form: [$(st 1)] [$(st 2)] [$(st 3)]"
	fi
	stopall
done
echo "--- D. a proxy cluster"
if [ -n "$SA" ]; then skp "D: the standalone edition refuses a [cluster] section"; else
	conf 1 proxy
	if start 1; then drive D 18911; else bad "D: node did not start"; fi
	stopall
fi
echo "setnxtest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
