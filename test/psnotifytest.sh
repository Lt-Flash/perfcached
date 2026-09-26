#!/bin/sh
# psnotifytest.sh - the native doors' notification is built once per
# message, and once per message under each pattern, and every subscriber
# still gets its own.  The body was rebuilt for every subscriber - 45% of
# the JSON door's CPU delivering to 64 of them - although it depends only
# on the message and the pattern it matched.  Now the first native
# subscriber a worker meets builds it and the rest reuse it; this pins
# what reuse must not change.
#   - one worker, so every subscriber shares one delivery loop
#   - on channel t: two text-dialect and one binary-dialect subscriber
#   - on patterns t* and * (text) and t* (binary): each must name ITS
#     pattern, so a body shared across patterns shows as the wrong one
#   - payloads: plain, one that needs JSON escaping, and binary (base64
#     with the enc sibling); then a channel only the patterns match
#   - RESP subscribers on the same worker still get RESP frames
# Usage: test/psnotifytest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcnotify.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 1
[memory]
arena_mb = 32
[secrets]
client = notify-client-secret
[listen]
tcp = 127.0.0.1:17601
resp = 127.0.0.1:17602
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(tail -1 "$D/n.log")"; echo "psnotifytest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 60 python3 - <<'PY' 2> "$D/py.err"
import base64, json, os, socket, struct
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()

class Text:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 17601), timeout=3)
        self.f = self.s.makefile("rb")
    def call(self, method, params):
        self.s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}) + "\n").encode())
        return json.loads(self.f.readline())
    def note(self):
        try:
            return json.loads(self.f.readline())
        except (socket.timeout, ValueError):
            return None

class Binary:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 17601), timeout=3)
    def call(self, verb, name):
        pl = bytes([verb]) + struct.pack("<H", len(name)) + name
        self.s.sendall(bytes([0x9E, 1, 1, 0]) + struct.pack("<I", len(pl)) + struct.pack("<Q", 5) + pl)
        return self.frame()
    def frame(self):
        try:
            h = b""
            while len(h) < 16:
                h += self.s.recv(16 - len(h))
            n = struct.unpack("<I", h[4:8])[0]
            d = b""
            while len(d) < n:
                d += self.s.recv(n - len(d))
            return h[2], d
        except socket.timeout:
            return None, None
    def note(self):
        typ, d = self.frame()
        return json.loads(d) if typ == 3 else None

class Resp:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 17602), timeout=3)
        self.f = self.s.makefile("rb")
    def send(self, *args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            a = a if isinstance(a, bytes) else a.encode()
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        self.s.sendall(out)
    def read(self):
        line = self.f.readline()
        if line[:1] == b":":
            return int(line[1:])
        if line[:1] == b"*":
            items = []
            for _ in range(int(line[1:])):
                items.append(self.read())
            return items
        if line[:1] == b"$":
            n = int(line[1:])
            v = self.f.read(n + 2)[:n]
            return v
        return line

t1, t2, tp, tall = Text(), Text(), Text(), Text()
b1, bp = Binary(), Binary()
check(t1.call("subscribe", {"channel": "t"}).get("result") == {"subscribed": 1}, "text subscriber 1 on t")
check(t2.call("subscribe", {"channel": "t"}).get("result") == {"subscribed": 1}, "text subscriber 2 on t")
check(tp.call("psubscribe", {"pattern": "t*"}).get("result") == {"subscribed": 1}, "text subscriber on pattern t*")
check(tall.call("psubscribe", {"pattern": "*"}).get("result") == {"subscribed": 1}, "text subscriber on pattern *")
typ, d = b1.call(11, b"t")
check(typ == 2 and b'"subscribed":1' in d, "binary subscriber on t")
typ, d = bp.call(13, b"t*")
check(typ == 2 and b'"subscribed":1' in d, "binary subscriber on pattern t*")
rs = Resp()
rs.send("SUBSCRIBE", "t")
check(rs.read() == [b"subscribe", b"t", 1], "RESP subscriber on t")
pub = Resp()

def expect_all(chan, payload, wire):
    pub.send("PUBLISH", chan, payload)
    n = pub.read()
    chan_subs = chan == "t"
    want = (4 if chan_subs else 0) + 3
    check(n == want, "PUBLISH %s %r answers %d receivers (%r)" % (chan, payload[:12], want, n))
    msg = {"channel": chan}
    msg.update(wire)
    for name, sub in (("text 1", t1), ("text 2", t2), ("binary", b1)):
        got = sub.note() if chan_subs else None
        if chan_subs:
            check(got is not None and got.get("method") == "message" and "id" not in got and got.get("params") == msg,
                  "%s on t gets its message: %r" % (name, got))
    for name, sub, pat in (("text t*", tp, "t*"), ("text *", tall, "*"), ("binary t*", bp, "t*")):
        got = sub.note()
        want_p = {"pattern": pat}
        want_p.update(msg)
        check(got is not None and got.get("method") == "pmessage" and got.get("params") == want_p,
              "%s gets the pmessage naming %s: %r" % (name, pat, got))
    if chan_subs:
        frame = rs.read()
        pl = payload if isinstance(payload, bytes) else payload.encode()
        check(frame == [b"message", b"t", pl], "RESP subscriber gets its RESP frame (%r)" % (frame,))

expect_all("t", "hello", {"payload": "hello"})
expect_all("t", 'q"\\\n\tx', {"payload": 'q"\\\n\tx'})
expect_all("t", b"\x00\xffbin", {"payload": base64.b64encode(b"\x00\xffbin").decode(), "enc": "b64"})
expect_all("tx", "only-patterns", {"payload": "only-patterns"})
for name, sub in (("text 1", t1), ("text 2", t2)):
    sub.s.settimeout(0.3)
    check(sub.note() is None, "%s on t gets nothing for tx" % name)
b1.s.settimeout(0.3)
check(b1.note() is None, "binary on t gets nothing for tx")
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
echo "psnotifytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
