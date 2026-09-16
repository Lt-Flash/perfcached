#!/bin/sh
# pubsubrelaytest.sh - PS3: a publish on one node reaches the subscribers
# on every node of the fleet over the sealed unicast plane; PUBLISH still
# answers the LOCAL count; a relayed message is never relayed again and a
# sender never receives its own; the receiver counts sequence gaps; a
# payload over the datagram stays local.  Usage: test/pubsubrelaytest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpsrelay.XXXXXX); P1= P2=
trap 'for v in "$P1" "$P2"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { mkdir -p "$D/s$1"; cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = psrelay-client-secret
cluster = psrelay-cluster-secret
[listen]
tcp = 127.0.0.1:1748$1
resp = 127.0.0.1:1749$1
http = 127.0.0.1:1848$1
plaintext = loopback
[cluster]
multicast = 239.255.77.51:17151
advertise = 127.0.13.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/n$1.conf"; }
start() { "$BIN" -f "$D/n$1.conf" -D > "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 did not start:"; tail -3 "$D/n$1.log"; return 1; }
node 1; node 2; start 1 && start 2 || { echo "pubsubrelaytest: 0 passed, 1 failed"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	a=$(curl -s -m 3 http://127.0.0.1:18481/stats | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null)
	b=$(curl -s -m 3 http://127.0.0.1:18482/stats | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null)
	[ "$a" = "ready 1" ] && [ "$b" = "ready 1" ] && break; sleep 0.2; i=$((i+1)); done
[ "$a" = "ready 1" ] && [ "$b" = "ready 1" ] && ok "two nodes formed a fleet" || { bad "fleet never formed: [$a] [$b]"; echo "pubsubrelaytest: $pass passed, $((fail))"; exit 1; }

timeout 90 python3 - <<'PY' > "$D/py.out" 2>&1
import json, socket, time, urllib.request
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
class R:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5); self.buf = b""
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + (x if isinstance(x, bytes) else x.encode()) + b"\r\n" for x in a))
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
        if t == b"*": return [self.read() for _ in range(int(b))]
        return "<?>"
    def cmd(self, *a): self.send(*a); return self.read()
    def nothing(self, secs=0.5):
        self.s.settimeout(secs)
        try:
            d = self.s.recv(4096); self.s.settimeout(5); return len(d) == 0 and self.buf == b""
        except socket.timeout:
            self.s.settimeout(5); return self.buf == b""
def stats(port): return json.load(urllib.request.urlopen("http://127.0.0.1:%d/stats" % port, timeout=5)).get("pubsub", {})
S1, S2, P1, P2 = R(17491), R(17492), R(17491), R(17492)
ok(S1.cmd("SUBSCRIBE", "x") == ["subscribe", "x", 1] and S2.cmd("SUBSCRIBE", "x") == ["subscribe", "x", 1], "a subscriber on each node")
ok(P2.cmd("PUBLISH", "x", "m1") == 1, "PUBLISH on node 2 answers node 2's LOCAL count (1)")
ok(S2.read() == ["message", "x", "m1"], "node 2's subscriber receives it")
ok(S1.read() == ["message", "x", "m1"], "node 1's subscriber receives it through the relay")
ok(S2.nothing(), "node 2's subscriber gets it once - the sender never receives its own relay")
ok(P1.cmd("PUBLISH", "x", "m2") == 1, "PUBLISH on node 1 answers node 1's local count (1)")
ok(S1.read() == ["message", "x", "m2"] and S2.read() == ["message", "x", "m2"], "and both subscribers receive it")
ok(S1.cmd("PSUBSCRIBE", "y*") == ["psubscribe", "y*", 2], "a pattern subscription on node 1")
ok(P2.cmd("PUBLISH", "yes", "m3") == 0, "a publish on node 2 with no local subscriber answers 0 and still relays")
ok(S1.read() == ["pmessage", "y*", "yes", "m3"], "node 1's pattern subscriber receives the relayed publish")
for i in range(30):
    P2.cmd("PUBLISH", "x", "seq%d" % i)
got = [S1.read() for _ in range(30)]
ok(got == [["message", "x", "seq%d" % i] for i in range(30)], "30 relayed messages arrive in order")
for i in range(30): S2.read()
time.sleep(0.3)
s1, s2 = stats(18481), stats(18482)
ok(s2.get("relay_sent") == 32 and s1.get("relay_recv") == 32 and s1.get("relay_lost") == 0, "node 2 relayed 32, node 1 received 32, no gap (got sent=%r recv=%r lost=%r)" % (s2.get("relay_sent"), s1.get("relay_recv"), s1.get("relay_lost")))
ok(s1.get("relay_sent") == 1 and s2.get("relay_recv") == 1, "and node 1 relayed its one publish to node 2")
big = "b" * 65200
r = P2.cmd("PUBLISH", "x", big)
ok(r == 1, "a payload over the datagram is still published locally (count 1, got %r)" % (r if not isinstance(r, str) else r[:60]))
ok(S2.read() == ["message", "x", big], "node 2's subscriber receives the big payload")
ok(S1.nothing(0.7), "node 1's subscriber does not (over the datagram: local only)")
s2 = stats(18482)
ok(s2.get("relay_dropped") == 1, "and node 2 counts it as a relay dropped for size (got %r)" % s2.get("relay_dropped"))
m = urllib.request.urlopen("http://127.0.0.1:18482/metrics", timeout=5).read().decode()
ok("perfcached_pubsub_relay_sent_total 32" in m, "/metrics on node 2 carries relay_sent 32")
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the python driver did not finish: $(tail -3 $D/py.out | cut -c1-200)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + ${pf:-0}))
echo "pubsubrelaytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
