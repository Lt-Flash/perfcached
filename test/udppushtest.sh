#!/bin/sh
# udppushtest.sh - PS5: pub/sub pushes over UDP, from the daemon's side,
# with the datagrams opened here (libsodium through ctypes).
#   - pubsub_udp answers a stream and a key, and a probe arrives from the
#     door's own address and port, sealed with that key
#   - nothing but probes goes over UDP until the probe's cookie comes back;
#     a wrong cookie is refused
#   - then deliveries (message and pmessage) arrive as sealed datagrams in
#     sequence and not over TCP; one too large for a datagram stays on TCP
#   - an ack beyond what was sent is refused
#   - the destination is the connection's own peer, whatever the client says
#   - a client that stops acking, and one whose acks stop moving while
#     datagrams go out, lose every subscription and are told why
#   - an unconfirmed stream expires; port 0 stops a stream; a door with
#     pubsub_udp = no refuses
# Fail-first: the PS12 build has no pubsub_udp method.
# Then libperfd's side, through udppushcli: the same deliveries at the
# notify hook, blocking and async handles, gaps, a stranger's datagram,
# the timer acks, and a handle pruned for silence.
# Usage: test/udppushtest.sh [./perfcached] [./udppushcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-}
D=$(mktemp -d /var/tmp/pcudppush.XXXXXX); P1= P2=
trap 'for v in "$P1" "$P2"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
conf() { # conf <n> <tcp port> <http port> <pubsub_udp>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 32
[secrets]
client = udppush-client-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
pubsub_udp = $4
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/n$1.conf"; }
start() { # start <n>
	"$BIN" -f "$D/n$1.conf" -D > "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 did not start:"; tail -3 "$D/n$1.log"; return 1; }

conf 1 17491 18491 yes; conf 2 17495 18495 no
start 1 && start 2 || { echo "udppushtest: $pass passed, $((fail+1)) failed"; exit 1; }
grep -q "UDP pushes leave from 127.0.0.1:17491/udp" "$D/n1.log" && ok "the door logs the UDP address its pushes leave from" || bad "no UDP door line: $(grep -i udp "$D/n1.log" | head -2)"

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 150 python3 - <<'PY' 2> "$D/py.err"
import ctypes, ctypes.util, json, os, select, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m):
    res.write(("P " if c else "F ") + m + "\n"); res.flush()

so = None
for name in ("libsodium.so.23", "libsodium.so.26", "libsodium.so", ctypes.util.find_library("sodium")):
    try:
        so = ctypes.CDLL(name); break
    except Exception:
        pass
so.sodium_init()
def open_dg(key, dg):
    hdr, ct = dg[:19], dg[19:]
    out = ctypes.create_string_buffer(max(len(ct), 1)); olen = ctypes.c_ulonglong(0)
    rc = so.crypto_aead_chacha20poly1305_ietf_decrypt(out, ctypes.byref(olen), None, ct, ctypes.c_ulonglong(len(ct)),
        hdr, ctypes.c_ulonglong(19), b"\0\0\0\0" + hdr[11:19], key)
    return None if rc != 0 else out.raw[:olen.value]
def head(dg): return dg[0], dg[1], dg[2], dg[3:11].hex(), int.from_bytes(dg[11:19], "big")
def message(pt):
    kind = pt[0]; pl = int.from_bytes(pt[1:3], "big"); pat = pt[3:3 + pl]; o = 3 + pl
    cl = int.from_bytes(pt[o:o + 2], "big"); chan = pt[o + 2:o + 2 + cl]; o += 2 + cl
    dl = int.from_bytes(pt[o:o + 4], "big"); return kind, pat.decode(), chan.decode(), pt[o + 4:o + 4 + dl]

class J:
    def __init__(self, port=17491, src=None):
        self.s = socket.socket();
        if src: self.s.bind((src, 0))
        self.s.connect(("127.0.0.1", port)); self.buf = b""; self.notes = []; self.n = 0
    def line(self, timeout):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0 or not select.select([self.s], [], [], left)[0]: return None
            d = self.s.recv(65536)
            if not d: return None
            self.buf += d
        l, self.buf = self.buf.split(b"\n", 1); return json.loads(l)
    def call(self, method, params=None):
        self.n += 1
        req = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None: req["params"] = params
        self.s.sendall((json.dumps(req) + "\n").encode())
        while True:
            r = self.line(5)
            if r is None: raise Exception("no reply to " + method)
            if "id" in r and r["id"] == self.n: return r
            self.notes.append(r)
    def pump(self, timeout):
        while True:
            r = self.line(timeout)
            if r is None: return
            self.notes.append(r); timeout = 0.05
def udp(addr, port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind((addr, port)); u.settimeout(0.2); return u
def drain(u, secs):
    got, end = [], time.time() + secs
    while time.time() < end:
        try: got.append(u.recvfrom(4096))
        except socket.timeout: pass
    return got
def stats(port=18491): return json.load(urllib.request.urlopen("http://127.0.0.1:%d/stats" % port, timeout=5))["pubsub"]

pub = J()
c = J(); c.call("subscribe", {"channel": "u"}); c.call("psubscribe", {"pattern": "p.*"})
u1 = udp("127.0.0.1", 17493)
r = c.call("pubsub_udp", {"port": 17493})["result"]
key = bytes.fromhex(r["key"]); stream = r["stream"]
check(len(key) == 32 and len(stream) == 16 and r["max_datagram"] == 1400 and r["ack_every"] == 256 and r["ack_ms"] == 5000 and r["prune_ms"] == 15000,
      "pubsub_udp answers a stream, a 32-byte key and the ack contract (%s)" % {k: v for k, v in r.items() if k != "key"})
dg, src = u1.recvfrom(4096) if select.select([u1], [], [], 2)[0] else (b"", None)
check(src == ("127.0.0.1", 17491), "the probe comes from the door's own address and port (%s)" % (src,))
magic, ver, typ, sid, seq = head(dg) if len(dg) >= 19 else (0, 0, 0, "", 0)
pt = open_dg(key, dg)
check(magic == 0x70 and ver == 1 and typ == 1 and sid == stream and seq == 1 and pt is not None and len(pt) == 16,
      "it is probe 1 of this stream, sealed with its key, carrying a 16-byte cookie")
cookie = pt or b""
bad_dg = bytearray(dg); bad_dg[-20] ^= 1
check(open_dg(key, bytes(bad_dg)) is None and open_dg(bytes(32), dg) is None, "a flipped byte or another key does not open it")

pub.call("publish", {"channel": "u", "payload": "before"})
c.pump(0.5)
got = [open_dg(key, d) for d, _ in drain(u1, 0.5)]
check(any(n.get("method") == "message" and n["params"]["payload"] == "before" for n in c.notes) and all(g is not None and len(g) == 16 for g in got),
      "before the cookie comes back a delivery stays on TCP and only probes go over UDP (%d probes more)" % len(got))
e = c.call("pubsub_udp_confirm", {"cookie": "00" * 16})
check("error" in e, "a wrong cookie is refused (%s)" % e.get("error", {}).get("message"))
cr = c.call("pubsub_udp_confirm", {"cookie": cookie.hex()}).get("result", {})
check(cr.get("udp") is True and cr.get("seq", 0) >= 1, "the probe's cookie confirms the stream and says where messages start (%s)" % cr)
confirm_seq = cr.get("seq", 0)
drain(u1, 0.3); c.notes = []

for i in range(50): pub.call("publish", {"channel": "u", "payload": "m%d" % i})
for i in range(10): pub.call("publish", {"channel": "p.x", "payload": "q%d" % i})
msgs, seqs = [], []
for d, s in drain(u1, 1.0):
    h = head(d); o = open_dg(key, d)
    if h[2] == 2 and o is not None: msgs.append(message(o)); seqs.append(h[4])
c.pump(0.3)
check(len(msgs) == 60 and [m[3] for m in msgs[:50]] == [("m%d" % i).encode() for i in range(50)] and all(m[0] == 0 and m[2] == "u" for m in msgs[:50]),
      "50 publishes arrive as 50 sealed datagrams, in order (%d)" % len(msgs))
check(all(m[0] == 1 and m[1] == "p.*" and m[2] == "p.x" for m in msgs[50:]), "pattern deliveries carry the pattern")
check(seqs == list(range(confirm_seq + 1, confirm_seq + 1 + len(seqs))), "their sequence numbers run on from the confirmation without a gap")
check(not [n for n in c.notes if n.get("method") in ("message", "pmessage")], "and none of them came over TCP (%d)" % len(c.notes))
last = seqs[-1] if seqs else 0

pub.call("publish", {"channel": "u", "payload": "x" * 2000})
c.pump(0.5)
big = [n for n in c.notes if n.get("method") == "message" and len(n["params"]["payload"]) == 2000]
check(len(big) == 1 and not drain(u1, 0.3) and stats()["udp"]["oversized_to_tcp"] == 1,
      "a 2,000-byte payload does not fit a datagram and comes over TCP")
e = c.call("pubsub_udp_ack", {"seq": last + 100})
check("error" in e, "an ack beyond the last datagram is refused")
check(c.call("pubsub_udp_ack", {"seq": last}).get("result") == {"acked": last}, "an ack of the last one is taken")

# the destination is the connection's peer, whatever else is said
c2 = J(src="127.0.0.2")
u2a, u2b = udp("127.0.0.2", 17494), udp("127.0.0.1", 17494)
c2.call("pubsub_udp", {"port": 17494, "host": "127.0.0.1", "ip": "127.0.0.1"})
a, b = drain(u2a, 1.0), drain(u2b, 0.2)
check(len(a) >= 1 and not b, "a connection from 127.0.0.2 gets its probes at 127.0.0.2 only (%d there, %d at 127.0.0.1)" % (len(a), len(b)))

# the prunes: c's acks keep coming but stop moving while publishes go out;
# c3 confirms and never acks; c4 never confirms
c3 = J(); c3.call("subscribe", {"channel": "s3"}); u3 = udp("127.0.0.1", 17496)
k3 = bytes.fromhex(c3.call("pubsub_udp", {"port": 17496})["result"]["key"])
ck3 = open_dg(k3, u3.recvfrom(4096)[0]) if select.select([u3], [], [], 2)[0] else b""
c3.call("pubsub_udp_confirm", {"cookie": ck3.hex()})
c4 = J(); u4 = udp("127.0.0.1", 17497)
k4 = bytes.fromhex(c4.call("pubsub_udp", {"port": 17497})["result"]["key"])
t0 = time.time(); probes4 = 0; next_ack = t0; pruned = {}
c.notes = []; c3.notes = []
while time.time() - t0 < 22 and len(pruned) < 2:
    pub.call("publish", {"channel": "u", "payload": "tick"})
    if time.time() >= next_ack:
        c.call("pubsub_udp_ack", {"seq": last}); next_ack += 2
    for who, conn in (("c", c), ("c3", c3)):
        conn.pump(0.01)
        for n in conn.notes:
            if n.get("method") == "pubsub_udp_pruned" and who not in pruned:
                pruned[who] = (round(time.time() - t0, 1), n["params"]["reason"])
    try:
        while True:
            d, _ = u4.recvfrom(4096); probes4 += open_dg(k4, d) is not None
    except socket.timeout:
        pass
    time.sleep(0.1)
check(pruned.get("c", (0, ""))[1] == "no progress" and 14 <= pruned["c"][0] <= 18,
      "acks that stop moving while datagrams go out: pruned after ~15 s, reason no progress (%s)" % (pruned.get("c"),))
check(pruned.get("c3", (0, ""))[1] == "no ack" and 14 <= pruned["c3"][0] <= 18,
      "a confirmed stream that is never acked: pruned after ~15 s, reason no ack (%s)" % (pruned.get("c3"),))
check(pub.call("publish", {"channel": "u", "payload": "after"})["result"]["receivers"] == 0 and
      pub.call("publish", {"channel": "p.y", "payload": "after"})["result"]["receivers"] == 0 and
      pub.call("publish", {"channel": "s3", "payload": "after"})["result"]["receivers"] == 0,
      "a pruned connection has lost every subscription, channel and pattern")
check(c.call("subscribe", {"channel": "again"}).get("result") == {"subscribed": 1}, "and stays open: it may subscribe again")
time.sleep(max(0, 11 - (time.time() - t0)))
s = stats()["udp"]
check(s["expired"] >= 1 and "error" in c4.call("pubsub_udp_confirm", {"cookie": "00" * 16}),
      "a stream never confirmed expires after 10 s (expired %d)" % s["expired"])
check(s["pruned_no_ack"] == 1 and s["pruned_no_progress"] == 1 and s["confirmed"] == 2 and s["pushed"] >= 60,
      "/stats counts the prunes, confirmations and datagrams (%s)" % s)

met = urllib.request.urlopen("http://127.0.0.1:18491/metrics", timeout=5).read().decode()
check('perfcached_pubsub_udp_pruned_total{reason="no_ack"} 1' in met and 'perfcached_pubsub_udp_pruned_total{reason="no_progress"} 1' in met and
      "perfcached_pubsub_udp_oversized_total 1" in met, "/metrics carries the prunes by reason and the oversized deliveries")
c5 = J(); u5 = udp("127.0.0.1", 17498)
before = stats()["udp"]["streams"]
c5.call("pubsub_udp", {"port": 17498})
check(c5.call("pubsub_udp", {"port": 0}).get("result") == {"udp": False} and stats()["udp"]["streams"] == before, "port 0 stops a stream")
e = J(17495).call("pubsub_udp", {"port": 17499})
check("error" in e and "no UDP pushes" in e["error"]["message"], "a door with pubsub_udp = no refuses (%s)" % e.get("error", {}).get("message"))

# probes on a quiet daemon: nothing else may wake the stream's worker, so
# the worker's own clock has to send probes 2 and 3
time.sleep(1.5)
c6 = J(); u6 = udp("127.0.0.1", 17490)
k6 = bytes.fromhex(c6.call("pubsub_udp", {"port": 17490})["result"]["key"])
seen = []
t6 = time.time()
while time.time() - t6 < 4.5:
    if select.select([u6], [], [], 0.2)[0]:
        d, _ = u6.recvfrom(4096)
        if open_dg(k6, d) is not None: seen.append((round(time.time() - t6, 1), head(d)[4]))
check(len(seen) == 3 and [x[1] for x in seen] == [1, 2, 3] and 0.7 <= seen[1][0] <= 2.2 and seen[2][0] <= 3.5,
      "on an idle daemon an unconfirmed stream still gets its 3 probes, about a second apart (%s)" % seen)
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -5 "$D/py.err"; }
grep -q "UDP pushes to 127.0.0.1 pruned (no ack)" "$D/n1.log" && ok "the prune is logged with its reason" || bad "no prune line in the log"
if [ -n "$CLI" ]; then
	if [ -x "$CLI" ]; then
		timeout 90 "$CLI" 17491 17495 > "$D/cli.out" 2>&1
		while IFS= read -r l; do case "$l" in "  ok   "*) ok "libperfd: ${l#  ok   }" ;; "  FAIL "*) bad "libperfd: ${l#  FAIL }" ;; esac; done < "$D/cli.out"
		grep -q "^udppushcli:" "$D/cli.out" || bad "udppushcli did not finish: $(tail -2 "$D/cli.out" | tr '\n' ' ')"
	else
		bad "no udppushcli at $CLI"
	fi
fi
echo "udppushtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
