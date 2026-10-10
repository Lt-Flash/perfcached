#!/bin/sh
# hsparktest.sh - ST5: a worker turn runs at most PC_HS_PER_TURN Noise
# handshakes and parks the rest, oldest first, off the epoll ready list - so
# an established client's request never waits behind a connect storm's
# backlog (1 worker, ~6,000 encrypted connects/s: a RESP probe's p99 89-107
# ms before, 33-37 ms with parking and the accept cap; DESIGN 12kw).
# Latency is the measurement's job (hold50k/st5bench.sh); this suite proves
# the mechanism and that it loses nothing: ONE worker, 400 encrypted clients
# connect and send their first handshake message in one burst, every one
# completes the handshake and a ping over the channel, and /stats says
# handshakes were parked (clients.handshakes.deferred > 0, per_turn 8).
# The initiator is test/noise_interop.py's (an independent Noise
# implementation); where the host's python cryptography is too old to drive
# it, the suite SKIPS loudly, as noiseinterop does.
# Fail-first: 0.5.6.6 has no clients.handshakes; a build whose cap never
# binds parks nothing (deferred 0).
# Usage: test/hsparktest.sh [./perfcached] [./noisetest]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
NT=${2:-./noisetest}
HERE=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pchspark.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
PW="hspark-client-secret"
PSK=$("$NT" psk client "$PW") || { echo "hsparktest: FAIL - psk derive"; exit 1; }
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 1
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = $PW
[listen]
tcp = 127.0.0.1:18571
http = 127.0.0.1:18573
plaintext = never
[collection 0]
buckets_log2 = 10
CONF
chmod 640 "$D/pc.conf"
ulimit -n "$(ulimit -H -n)" 2>/dev/null
"$BIN" -f "$D/pc.conf" > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "hsparktest: FAIL - daemon did not start"; tail -3 "$D/pc.log"; exit 1; }
timeout 120 python3 - "$HERE/noise_interop.py" "$PSK" > "$D/py.out" 2>&1 <<'PY'
import json, socket, struct, sys, urllib.request
src = open(sys.argv[1]).read().split("\ndef main():")[0]
g = {"__name__": "ni"}
exec(compile(src, sys.argv[1], "exec"), g)      # the primitives; exits 77 on an old library
import pcnative
psk = bytes.fromhex(sys.argv[2]); N = 400
Sym, NAME, X25519PrivateKey, X25519PublicKey = g["Sym"], g["NAME"], g["X25519PrivateKey"], g["X25519PublicKey"]
ChaCha20Poly1305, nonce, recv_exact = g["ChaCha20Poly1305"], g["nonce"], g["recv_exact"]
conns = []
for i in range(N):
    s = socket.create_connection(("127.0.0.1", 18571), timeout=30)
    sym = Sym(NAME); sym.mix_hash(bytes([0])); sym.mix_key_and_hash(psk)
    e = X25519PrivateKey.generate(); ep = e.public_key().public_bytes_raw()
    sym.mix_hash(ep); sym.mix_key(ep)
    n1 = ep + sym.encrypt_and_hash(b"\x01")
    conns.append((s, sym, e, struct.pack("<H", 1 + len(n1)) + bytes([0]) + n1))
for s, sym, e, f1 in conns:                      # the burst: every msg1 at once
    s.sendall(f1)
ok = 0; bad = []
for i, (s, sym, e, f1) in enumerate(conns):
    try:
        h = recv_exact(s, 2)
        m2 = recv_exact(s, struct.unpack("<H", h)[0])
        re_ = m2[:32]; sym.mix_hash(re_); sym.mix_key(re_)
        sym.mix_key(e.exchange(X25519PublicKey.from_public_bytes(re_)))
        sym.decrypt_and_hash(m2[32:])
        sk, rk = sym.split()
        ct = ChaCha20Poly1305(sk).encrypt(nonce(0), pcnative.frame(1, "ping"), b"")
        s.sendall(struct.pack("<H", len(ct)) + ct)
        h = recv_exact(s, 2)
        pt = ChaCha20Poly1305(rk).decrypt(nonce(0), recv_exact(s, struct.unpack("<H", h)[0]), b"")
        r = pcnative.decode(pt[pcnative.HDR.size:])[0]
        if r.get("pong") is True:
            ok += 1
        else:
            bad.append((i, r))
    except Exception as ex:
        bad.append((i, type(ex).__name__ + ": " + str(ex)[:60]))
hs = json.loads(urllib.request.urlopen("http://127.0.0.1:18573/stats", timeout=5).read())["clients"].get("handshakes")
print("RESULT %d %s %s" % (ok, json.dumps(hs, separators=(",", ":")), bad[:3]))
PY
rc=$?
if [ $rc = 77 ] || grep -q "^SKIP" "$D/py.out"; then
	echo "  ..   SKIP: this host's python cryptography is too old to drive the Noise initiator"
	echo "hsparktest: SKIPPED"
	exit 0
fi
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
R=$(grep "^RESULT" "$D/py.out")
set -- $R
[ "${2:-0}" = 400 ] && ok "all 400 encrypted clients of a one-worker burst completed the handshake and a ping" \
	|| bad "completed ${2:-?} of 400: $R $(tail -3 "$D/py.out" | tr '\n' ' ')"
DEF=$(echo "${3:-}" | python3 -c 'import json,sys; h=json.loads(sys.stdin.read() or "null"); print(-1 if not h else h.get("deferred", -1), -1 if not h else h.get("per_turn", -1))' 2>/dev/null)
set -- $DEF
[ "${1:--1}" -gt 0 ] 2>/dev/null && [ "${2:-}" = 8 ] \
	&& ok "handshakes were parked and run later (clients.handshakes.deferred $1, per_turn $2)" \
	|| bad "clients.handshakes: deferred ${1:-?}, per_turn ${2:-?} (want > 0, 8)"
grep -q -E " (ERROR|CRIT)" "$D/pc.log" && bad "ERROR/CRIT in the log: $(grep -m1 -E ' (ERROR|CRIT)' "$D/pc.log" | cut -c1-120)" \
	|| ok "no ERROR or CRIT in the log"
echo "hsparktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
