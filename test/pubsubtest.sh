#!/bin/sh
# pubsubtest.sh - PS1/PS2: pub/sub over RESP2 on one node.  Subscriptions
# live on the connection; a publish reaches every local subscriber, on
# whichever worker owns it; patterns match as redis matches them; a
# subscribed connection is gated to the pub/sub verbs; a subscriber that
# cannot keep up dies at the output cap; accepted sockets carry TCP
# keepalive.  Usage: test/pubsubtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./pubsubcli}                 # the libperfd client (PS4)
D=$(mktemp -d /var/tmp/pcpubsub.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 4
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = pubsub-client-secret
[listen]
tcp = 127.0.0.1:17471
resp = 127.0.0.1:17472
http = 127.0.0.1:18471
plaintext = loopback
keepalive_s = 120
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/pc.conf"
"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "  daemon did not start:"; tail -3 "$D/pc.log"; echo "pubsubtest: 0 passed, 1 failed"; exit 1; }

timeout 120 python3 - "$D" <<'PY' > "$D/py.out" 2>&1
import json, socket, sys, time, urllib.request
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
class R:
    def __init__(self, timeout=5):
        self.s = socket.create_connection(("127.0.0.1", 17472), timeout=timeout); self.f = self.s.makefile("rb")
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + (x if isinstance(x, bytes) else x.encode()) + b"\r\n" for x in a))
    def read(self):
        l = self.f.readline()
        if not l: return "<EOF>"
        t, b = l[:1], l[1:-2]
        if t == b"+": return b.decode()
        if t == b"-": return "ERR:" + b.decode()
        if t == b":": return int(b)
        if t == b"$":
            n = int(b)
            if n < 0: return None
            d = self.f.read(n + 2)[:-2]; return d.decode(errors="replace")
        if t == b"*":
            n = int(b); return [self.read() for _ in range(n)]
        return "<?>" + l.decode()
    def cmd(self, *a): self.send(*a); return self.read()
    def close(self): self.f.close(); self.s.close()   # the makefile holds the socket open
A, B, C, Dd = R(), R(), R(), R()
ok(A.cmd("SUBSCRIBE", "news") == ["subscribe", "news", 1], "SUBSCRIBE confirms with the running count")
ok(B.cmd("PUBLISH", "news", "hello") == 1, "PUBLISH returns the local receiver count (1)")
ok(A.read() == ["message", "news", "hello"], "the subscriber receives ['message', channel, payload]")
ok(C.cmd("PSUBSCRIBE", "n*") == ["psubscribe", "n*", 1], "PSUBSCRIBE confirms")
ok(B.cmd("PUBLISH", "news", "hello2") == 2, "a publish now counts the pattern subscriber too (2)")
ok(A.read() == ["message", "news", "hello2"], "exact subscriber gets message")
ok(C.read() == ["pmessage", "n*", "news", "hello2"], "pattern subscriber gets ['pmessage', pattern, channel, payload]")
ok(B.cmd("PUBSUB", "NUMSUB", "news", "nothing") == ["news", 1, "nothing", 0], "PUBSUB NUMSUB counts exact subscribers, O(1) by name")
ok(B.cmd("PUBSUB", "NUMPAT") == 1, "PUBSUB NUMPAT")
ok(B.cmd("PUBSUB", "CHANNELS") == ["news"], "PUBSUB CHANNELS lists channels with subscribers")
ok(B.cmd("PUBSUB", "CHANNELS", "x*") == [], "PUBSUB CHANNELS with a non-matching glob is empty")
r = A.cmd("GET", "k")
ok(isinstance(r, str) and r.startswith("ERR:ERR Can't execute 'get': only (P|S)SUBSCRIBE"), "subscribed mode refuses a data command with redis's text (got %r)" % r)
ok(A.cmd("PING") == ["pong", ""], "PING in subscribed mode answers the array form")
ok(A.cmd("PING", "hi") == ["pong", "hi"], "PING with a message echoes it in the array")
ok(A.cmd("UNSUBSCRIBE") == ["unsubscribe", "news", 0], "bare UNSUBSCRIBE names each channel it drops with the count left")
ok(B.cmd("PUBLISH", "news", "z") == 1, "after the unsubscribe only the pattern subscriber is counted")
ok(C.read() == ["pmessage", "n*", "news", "z"], "and receives it")
ok(A.cmd("GET", "k") is None, "out of subscribed mode a data command works again (GET nil)")
ok(A.cmd("UNSUBSCRIBE") == ["unsubscribe", None, 0], "bare UNSUBSCRIBE with nothing subscribed answers a nil channel")
ok(Dd.cmd("SUBSCRIBE", "a", "b", "c") == ["subscribe", "a", 1], "SUBSCRIBE a b c: first confirmation")
ok(Dd.read() == ["subscribe", "b", 2] and Dd.read() == ["subscribe", "c", 3], "  ...then counts 2 and 3")
ok(Dd.cmd("UNSUBSCRIBE", "b") == ["unsubscribe", "b", 2], "UNSUBSCRIBE b leaves 2")
r = B.cmd("PUBLISH", "__pc.event", "x")
ok(isinstance(r, str) and "reserved" in r, "PUBLISH into the __pc. prefix is refused")
ok(B.cmd("PUBLISH", "zzz", "x") == 0, "PUBLISH with no subscriber returns 0")
ok(Dd.cmd("RESET") == "RESET", "RESET answers +RESET")
ok(Dd.cmd("GET", "k") is None, "  ...and leaves subscribed mode")
r = Dd.cmd("HELLO", "3")
ok(isinstance(r, str) and r.startswith("ERR:NOPROTO"), "HELLO 3 is refused (RESP2 only)")
# pattern semantics, redis's stringmatchlen
P = R()
for pat in ("h[ae]llo", "h[^e]llo", "h\\*llo", "h?llo", "[a-c]x", "a*c", "zed*", "*"):
    P.cmd("PSUBSCRIBE", pat)
vec = [("h[ae]llo", "hallo", 1), ("h[ae]llo", "hillo", 0), ("h[^e]llo", "hallo", 1), ("h[^e]llo", "hello", 0),
       ("h\\*llo", "h*llo", 1), ("h\\*llo", "hxllo", 0), ("h?llo", "hxllo", 1), ("h?llo", "hllo", 0),
       ("[a-c]x", "bx", 1), ("[a-c]x", "dx", 0), ("a*c", "abbbc", 1), ("a*c", "ac", 1), ("a*c", "abd", 0),
       # a trailing '*' matches the EMPTY tail, as redis does: news* matches
       # news itself, not only news-something.  Found by mutation: an engine
       # that required one character here passed every vector above.  The
       # channels stay off n* so the pattern subscriber C is not disturbed.
       ("zed*", "zed", 1), ("zed*", "zedx", 1), ("zed*", "ze", 0), ("*", "zq", 1)]
good = True
for pat, ch, want in vec:
    # the publish count says how many pattern subscriptions matched, so read
    # exactly that many pmessages and look for this vector's pattern
    n = B.cmd("PUBLISH", ch, "v")
    seen = set()
    for _ in range(n):
        m = P.read()
        if isinstance(m, list) and m[0] == "pmessage" and m[2] == ch:
            seen.add(m[1])
    got = 1 if pat in seen else 0
    if got != want:
        good = False; res.append(("FAIL", "pattern %r vs %r: want %d got %d (publish count %s, seen %r)" % (pat, ch, want, got, n, sorted(seen))))
ok(good, "pattern matching agrees with redis on the vectors")
P.close()
# one connection subscribed by channel AND by a matching pattern gets BOTH
# frames and is counted twice - redis's own behaviour, the count is
# deliveries, not distinct clients
Dup = R(); Dup.cmd("SUBSCRIBE", "dupch"); Dup.cmd("PSUBSCRIBE", "dup*")
ok(B.cmd("PUBLISH", "dupch", "d1") == 2, "a connection matched by channel and pattern counts twice in PUBLISH's answer")
f1, f2 = Dup.read(), Dup.read()
ok(sorted([f1[0], f2[0]]) == ["message", "pmessage"] and f1[-1] == "d1" and f2[-1] == "d1",
   "and receives a message frame and a pmessage frame (%r, %r)" % (f1, f2))
Dup.close()
# a hostile pattern must not stall the engine: the first matcher was
# exponential in '*' and ran for minutes under the engine lock; the engine's
# own matcher is O(pattern x channel).  And an OPEN set must not read past
# the pattern: "[a" is closed at the pattern's end, so it matches "a" only.
H = R(timeout=10); H.cmd("PSUBSCRIBE", "*a" * 16 + "*b"); H.cmd("PSUBSCRIBE", "[q")
B.s.settimeout(3)
t0 = time.time()
try:
    n = B.cmd("PUBLISH", "a" * 200, "x")
    dt = time.time() - t0
    ok(n == 0 and dt < 1.0, "a hostile 16-star pattern against a 200-byte channel: PUBLISH answers in %.3f s" % dt)
except Exception as e:
    ok(False, "a hostile 16-star pattern stalled PUBLISH past 3 s (%s)" % e)
ok(B.cmd("PUBLISH", "q", "o1") == 1 and B.cmd("PUBLISH", "qq", "o2") == 0,
   "an open set [q is closed at the pattern end: it matches q and not qq")
B.s.settimeout(5)
H.close()
# cross-worker fan-out: 12 subscribers over 4 workers
subs = [R() for _ in range(12)]
for s in subs: s.cmd("SUBSCRIBE", "fan")
time.sleep(0.2)
ok(B.cmd("PUBLISH", "fan", "one") == 12, "12 subscribers over 4 workers are counted (12)")
got = sum(1 for s in subs if s.read() == ["message", "fan", "one"])
ok(got == 12, "and every one of them receives it (%d of 12)" % got)
for s in subs[:6]: s.close()
time.sleep(0.5)
ok(B.cmd("PUBSUB", "NUMSUB", "fan") == ["fan", 6], "closed subscribers leave the table (6 left)")
ok(B.cmd("PUBLISH", "fan", "two") == 6, "and a publish counts the six that remain")
st = json.load(urllib.request.urlopen("http://127.0.0.1:18471/stats", timeout=5)).get("pubsub", {})
ok(st.get("subscribers") == 6 + 1 and st.get("channels") == 1 and st.get("patterns") == 1 and st.get("published") >= 15 and st.get("delivered") >= 30, "/stats pubsub block reads 7 subscribers, 1 channel, 1 pattern (got %r)" % st)
m = urllib.request.urlopen("http://127.0.0.1:18471/metrics", timeout=5).read().decode()
ok("perfcached_pubsub_subscribers 7" in m and "perfcached_pubsub_published_total" in m, "/metrics carries the subscriber gauge and the counters")
# slow subscriber: never reads; 8 MB cap
S = R(timeout=10); S.cmd("SUBSCRIBE", "big")
# PIPELINED, so one read on the delivering worker carries many publishes and
# several of them are handed to the subscriber after the cap has closed it -
# which is what a real publisher does, and what made one slow subscriber
# count thousands of kills.  A one-at-a-time loop let the close finish
# between publishes and could not tell the two apart.
payload = "x" * 4096
# the cap is 8 MB of staged output, but out + wire + the kernel's send and
# receive buffers absorb ~26 MB before it can bite: send ~49 MB
N = 12000
frame = ("*3\r\n$7\r\nPUBLISH\r\n$3\r\nbig\r\n$%d\r\n%s\r\n" % (len(payload), payload)).encode()
B.s.settimeout(30)
B.s.sendall(frame * N)
replies = [B.read() for _ in range(N)]
B.s.settimeout(5)
# no assertion on WHEN the counts drop to 0: a subscriber that shares the
# publisher's worker is torn down only after the whole pipeline is read, so
# every publish still counts it - redis closes over-limit clients
# asynchronously too.  What is promised is below: one kill, counted once.
ok(replies[0] == 1, "the pipelined publishes reach the subscriber (%d of %d counted it)" % (replies.count(1), N))
S.s.settimeout(5)
try:
    S.s.recv(1)  # we did not read the frames; the daemon should have shut us
    # drain until EOF
    end = time.time() + 5; eof = False
    while time.time() < end:
        d = S.s.recv(1 << 20)
        if not d: eof = True; break
except Exception as e:
    eof = "timeout" not in str(e).lower()
ok(eof, "a subscriber that never reads is closed at the output cap")
time.sleep(0.5)
st = json.load(urllib.request.urlopen("http://127.0.0.1:18471/stats", timeout=5)).get("pubsub", {})
ok(st.get("slow_kills") == 1, "and counted as ONE slow kill, however many messages were still handed to it while it closed (%r)" % st.get("slow_kills"))
ok(st.get("alloc_failed") == 0, "and no allocation failure was counted for it (%r)" % st.get("alloc_failed"))
# ---- PS4: the native doors ----
J = socket.create_connection(("127.0.0.1", 17471), timeout=5); jf = J.makefile("rb")
def jcmd(obj):
    J.sendall((json.dumps(obj) + "\n").encode()); return json.loads(jf.readline())
r = jcmd({"jsonrpc": "2.0", "id": 1, "method": "subscribe", "params": {"channel": "j"}})
ok(r.get("result") == {"subscribed": 1}, "JSON door: subscribe answers {subscribed: 1} (got %r)" % r)
ok(B.cmd("PUBLISH", "j", "from-resp") == 1, "a RESP publish counts the JSON subscriber")
r = json.loads(jf.readline())
ok(r.get("method") == "message" and r.get("params") == {"channel": "j", "payload": "from-resp"} and "id" not in r, "JSON door receives an id-less message notification (got %r)" % r)
r = jcmd({"jsonrpc": "2.0", "id": 2, "method": "get", "params": {"col": "0", "key": "nokey"}})
ok(r.get("id") == 2 and "result" in r, "a data call on the subscribed JSON connection still works: multiplexed")
r = jcmd({"jsonrpc": "2.0", "id": 3, "method": "publish", "params": {"channel": "news", "payload": "from-json"}})
ok(r.get("result") == {"receivers": 1}, "JSON door: publish answers {receivers: 1} (C's n* pattern)")
ok(C.read() == ["pmessage", "n*", "news", "from-json"], "and the RESP pattern subscriber receives it")
ok(B.cmd("PUBLISH", "j", b"\x00\xffbin") == 1, "a binary payload to the JSON subscriber")
r = json.loads(jf.readline())
ok(r.get("params", {}).get("enc") == "b64" and r["params"].get("payload"), "arrives base64 with the enc sibling (got %r)" % r.get("params"))
r = jcmd({"jsonrpc": "2.0", "id": 4, "method": "unsubscribe"})
ok(r.get("result") == {"subscribed": 0}, "JSON door: bare unsubscribe answers {subscribed: 0}")
jf.close(); J.close()
import struct
def bframe(verb, name, data=b""):
    pl = bytes([verb]) + struct.pack("<H", len(name)) + name + data
    return bytes([0x9E, 1, 1, 0]) + struct.pack("<I", len(pl)) + struct.pack("<Q", 77) + pl
Bn = socket.create_connection(("127.0.0.1", 17471), timeout=5)
def bread():
    h = b""
    while len(h) < 16: h += Bn.recv(16 - len(h))
    typ, plen, rid = h[2], struct.unpack("<I", h[4:8])[0], struct.unpack("<Q", h[8:16])[0]
    d = b""
    while len(d) < plen: d += Bn.recv(plen - len(d))
    return typ, rid, d
Bn.sendall(bframe(11, b"bn")); typ, rid, d = bread()
ok(typ == 2 and rid == 77 and b'"subscribed":1' in d, "binary door: SUBSCRIBE verb answers a RSP frame with the id and {subscribed:1} (got %r %r)" % (typ, d[:40]))
ok(B.cmd("PUBLISH", "bn", "to-bin") == 1, "a RESP publish counts the binary subscriber")
typ, rid, d = bread()
ok(typ == 3 and b'"method":"message"' in d and b'"payload":"to-bin"' in d, "binary door receives a NOTIFY frame carrying the JSON message (got %r %r)" % (typ, d[:60]))
Bn.sendall(bframe(10, b"news", b"from-bin")); typ, rid, d = bread()
ok(typ == 2 and b'"receivers":1' in d, "binary door: PUBLISH verb answers {receivers: 1}")
ok(C.read() == ["pmessage", "n*", "news", "from-bin"], "and the RESP pattern subscriber receives it")
Bn.sendall(bframe(12, b"")); typ, rid, d = bread()
ok(b'"subscribed":0' in d, "binary door: UNSUBSCRIBE with clen 0 drops all")
Bn.close()
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the python driver did not finish: $(tail -3 $D/py.out)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + ${pf:-0}))
# keepalive on accepted sockets: the kernel shows the timer on the daemon's side
K=$(python3 -c '
import socket, time
s = socket.create_connection(("127.0.0.1", 17472)); s.sendall(b"*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nka\r\n"); time.sleep(0.3)
import subprocess; out = subprocess.run(["ss", "-tno", "state", "established", "( sport = :17472 )"], capture_output=True, text=True).stdout
print("keepalive" in out); s.close()' 2>/dev/null)
[ "$K" = True ] && ok "accepted sockets carry TCP keepalive ([listen] keepalive_s)" || bad "no keepalive timer on the daemon's side of a subscriber socket"
# ---- PS4: through libperfd ----
if [ -x "$CLI" ]; then
	"$CLI" 17471 > "$D/cli.out" 2>&1; crc=$?
	grep -E "^  (ok|FAIL|\()" "$D/cli.out"
	pass=$((pass + $(grep -c "^  ok " "$D/cli.out"))); fail=$((fail + $(grep -c "^  FAIL " "$D/cli.out")))
	[ $crc -eq 0 ] || [ "$(grep -c '^  FAIL ' "$D/cli.out")" -gt 0 ] || bad "pubsubcli exited $crc: $(tail -2 $D/cli.out)"
else
	bad "no libperfd client at $CLI (make pubsubcli)"
fi
echo "pubsubtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
