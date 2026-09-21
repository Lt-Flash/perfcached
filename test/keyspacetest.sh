#!/bin/sh
# keyspacetest.sh - PS8: keyspace notifications.  A collection with
# `notify_events` publishes __keyspace@<col>__:<key> (payload: the event)
# and __keyevent@<col>__:<event> (payload: the key) to THIS node's
# subscribers for what it applies - a set at the door, a del, a miss, an
# expiry from the sweep, a replica applied from a peer - and never
# relays them: on a two-node eager fleet a write on node 1 is one event
# on node 1 and one on node 2.  A collection without the mask says
# nothing.  Usage: test/keyspacetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pckeyspace.XXXXXX); P= P1= P2=
trap 'for v in "$P" "$P1" "$P2"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = keyspace-client-secret
[listen]
tcp = 127.0.0.1:17501
resp = 127.0.0.1:17502
http = 127.0.0.1:18501
plaintext = loopback
[collection 0]
buckets_log2 = 12
notify_events = all
[collection 1]
buckets_log2 = 12
CONF
chmod 640 "$D/pc.conf"
"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "  daemon did not start:"; tail -3 "$D/pc.log"; echo "keyspacetest: 0 passed, 1 failed"; exit 1; }
timeout 60 python3 - <<'PY' > "$D/py.out" 2>&1
import json, socket, time, urllib.request
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
class R:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5); self.buf = b""
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + x.encode() + b"\r\n" for x in a))
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
    def nothing(self, secs=0.7):
        self.s.settimeout(secs)
        try:
            d = self.s.recv(4096); self.s.settimeout(5); return len(d) == 0 and self.buf == b""
        except socket.timeout:
            self.s.settimeout(5); return self.buf == b""
S, C = R(17502), R(17502)
S.cmd("PSUBSCRIBE", "__keyspace@0__:*"); S.cmd("PSUBSCRIBE", "__keyevent@0__:*")
S.cmd("PSUBSCRIBE", "__keyspace@1__:*"); S.cmd("PSUBSCRIBE", "__keyevent@1__:*")
def two():
    a, b = S.read(), S.read()
    return sorted([a, b], key=lambda m: m[1])   # keyevent before keyspace
ok(C.cmd("SET", "k", "v") == "OK", "SET k on db 0")
ev = two()
ok(ev == [["pmessage", "__keyevent@0__:*", "__keyevent@0__:set", "k"], ["pmessage", "__keyspace@0__:*", "__keyspace@0__:k", "set"]], "a set is __keyevent@0__:set -> k and __keyspace@0__:k -> set (got %r)" % ev)
ok(C.cmd("DEL", "k") == 1, "DEL k")
ev = two()
ok(ev[0][2:] == ["__keyevent@0__:del", "k"] and ev[1][2:] == ["__keyspace@0__:k", "del"], "a del is both channels with 'del' (got %r)" % ev)
ok(C.cmd("GET", "nokey") is None, "GET of a missing key")
ev = two()
ok(ev[0][2:] == ["__keyevent@0__:keymiss", "nokey"] and ev[1][2:] == ["__keyspace@0__:nokey", "keymiss"], "a miss is 'keymiss' (got %r)" % ev)
ok(C.cmd("SET", "t", "v", "EX", "1") == "OK", "SET t with a 1 s TTL")
two()   # the set's pair
ev = two()   # the sweep's expiry, within the granularity
ok(ev[0][2:] == ["__keyevent@0__:expired", "t"] and ev[1][2:] == ["__keyspace@0__:t", "expired"], "the sweep's expiry is 'expired' (got %r)" % ev)
ok(C.cmd("SELECT", "1") == "OK" and C.cmd("SET", "q", "v") == "OK", "SET q on db 1, the collection without a mask")
ok(S.nothing(), "...and nothing is notified for it")
st = json.load(urllib.request.urlopen("http://127.0.0.1:18501/stats", timeout=5)).get("pubsub", {})
ok(st.get("keyspace_events") == 10 and st.get("relay_sent") == 0, "/stats counts 10 keyspace events and no relay (got %r)" % {k: st.get(k) for k in ("keyspace_events", "relay_sent")})
m = urllib.request.urlopen("http://127.0.0.1:18501/metrics", timeout=5).read().decode()
ok("perfcached_pubsub_keyspace_events_total 10" in m, "/metrics carries the counter")
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the python driver did not finish: $(tail -3 $D/py.out | cut -c1-200)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + ${pf:-0}))
kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=

echo "== two eager nodes: a write on node 1 is one event on each node, never relayed"
node() { mkdir -p "$D/s$1"; cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = keyspace-client-secret
cluster = keyspace-cluster-secret
[listen]
tcp = 127.0.0.1:1751$1
resp = 127.0.0.1:1752$1
http = 127.0.0.1:1851$1
plaintext = loopback
[cluster]
multicast = 239.255.77.52:17152
advertise = 127.0.14.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 12
notify_events = store, remove
CONF
	chmod 640 "$D/n$1.conf"; }
start() { "$BIN" -f "$D/n$1.conf" -D > "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done; return 1; }
node 1; node 2; start 1 && start 2 || { bad "fleet nodes did not start"; echo "keyspacetest: $pass passed, $((fail))"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	a=$(curl -s -m 3 http://127.0.0.1:18511/stats | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null)
	b=$(curl -s -m 3 http://127.0.0.1:18512/stats | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null)
	[ "$a" = "ready 1" ] && [ "$b" = "ready 1" ] && break; sleep 0.2; i=$((i+1)); done
[ "$a" = "ready 1" ] && [ "$b" = "ready 1" ] && ok "two nodes formed a fleet" || bad "fleet never formed: [$a] [$b]"
timeout 60 python3 - <<'PY' > "$D/py2.out" 2>&1
import socket, time, json, urllib.request
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
class R:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5); self.buf = b""
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + x.encode() + b"\r\n" for x in a))
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
    def count(self, secs=1.0):
        n = 0; self.s.settimeout(secs)
        try:
            while True:
                m = self.read(); n += 1
        except socket.timeout:
            pass
        self.s.settimeout(5); return n
S1, S2, W = R(17521), R(17522), R(17521)
S1.cmd("PSUBSCRIBE", "__keyevent@0__:set"); S2.cmd("PSUBSCRIBE", "__keyevent@0__:set")
ok(W.cmd("SET", "fk", "v") == "OK", "SET on node 1")
n1, n2 = S1.count(), S2.count()
ok(n1 == 1, "node 1's subscriber hears the door's set once (got %d)" % n1)
ok(n2 == 1, "node 2's subscriber hears the replica's set once - applied there, not relayed (got %d)" % n2)
st1 = json.load(urllib.request.urlopen("http://127.0.0.1:18511/stats", timeout=5)).get("pubsub", {})
st2 = json.load(urllib.request.urlopen("http://127.0.0.1:18512/stats", timeout=5)).get("pubsub", {})
ok(st1.get("relay_sent") == 0 and st2.get("relay_recv") == 0 and st1.get("keyspace_events") == 2 and st2.get("keyspace_events") == 2, "no relay for keyspace events, two per node (got %r / %r)" % ({k: st1.get(k) for k in ("relay_sent","keyspace_events")}, {k: st2.get(k) for k in ("relay_recv","keyspace_events")}))
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py2.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py2.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the fleet driver did not finish: $(tail -3 $D/py2.out | cut -c1-200)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py2.out"))); fail=$((fail + ${pf:-0}))
echo "keyspacetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
