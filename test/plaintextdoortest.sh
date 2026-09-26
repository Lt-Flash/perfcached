#!/bin/sh
# plaintextdoortest.sh - RV-2: no door off loopback reaches the
# transport's no-key copy-through.
#
# The transport copies a record through unencrypted when a cipher state
# has no key - that is Noise, and it is what `plaintext = loopback`
# rides on.  It is POLICY, not a hole: a native listener is plaintext-
# eligible only when the operator said `plaintext = loopback` AND the
# listener is on a loopback address (daemon.c listener_plaintext_ok);
# everything else starts a connection encrypted, so the first bytes must
# be a handshake.  Nothing tested that the policy holds at the door.
#
# One daemon, `plaintext = loopback`, a native listener on 127.0.0.1 and
# another on the host's own LAN address:
#  - a plaintext command on the loopback door is served (the control:
#    the policy is on, and the client here can be answered);
#  - the SAME plaintext write on the LAN door gets no result, and the
#    key it tried to write does not exist afterwards - read back through
#    the loopback door, so "not served" is a fact about the store, not
#    about what this client saw;
#  - an ENCRYPTED client on that LAN door is served (the door is alive;
#    what it refuses is plaintext).
# A second daemon with the default `plaintext = never`: the loopback
# door refuses plaintext too, and serves the encrypted client.
# Usage: test/plaintextdoortest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcpd.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
SEC=pd-client-secret

# the host's own LAN address - the door has to be off loopback for the
# policy to have anything to refuse
LANIP=$(ip -4 -o addr show scope global 2>/dev/null \
	| awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$LANIP" ] || { echo "plaintextdoortest: no global IPv4 address on this host - SKIPPED, the off-loopback door cannot be built"; exit 0; }
[ -x "$CLI" ] || { echo "plaintextdoortest: $CLI is missing (make perfcli) - the encrypted control cannot run"; exit 1; }

PLAIN="$D/plain.py"
cat > "$PLAIN" <<'EOF'
# one plaintext JSON-RPC line to host:port; prints the reply line, or why there is none
import json, socket, sys
try:
    s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=4)
    s.settimeout(3)
    s.sendall((sys.argv[3] + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(4096)
        if not d:
            break
        buf += d
    line = buf.split(b"\n")[0].decode("latin1")
    try:
        r = json.loads(line); print(json.dumps(r.get("result", r.get("error"))))
    except Exception:
        print("NOT-JSON(%d bytes)" % len(buf) if buf else "CLOSED")
except socket.timeout:
    print("TIMEOUT")
except Exception as e:
    print("ERR:%s" % type(e).__name__)
EOF
plain() { python3 "$PLAIN" "$1" "$2" "$3"; }
enc() { printf '%s\n' "$3" | timeout 20 "$CLI" -h "$1" -p "$2" -a $SEC -q 2>/dev/null | head -1; }

conf() { # conf <plaintext-policy-line>
	cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = $SEC
[listen]
tcp = 127.0.0.1:18301
tcp = $LANIP:18302
$1
[collection 0]
buckets_log2 = 10
EOF
}
start() {
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	P1=$!
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "the daemon did not start:"; tail -4 "$D/n.log"; exit 1
}
stop() { [ -n "$P1" ] && kill -9 $P1 2>/dev/null; P1=; sleep 0.5; }

SETL='{"jsonrpc":"2.0","id":1,"method":"set","params":{"col":"0","key":"via-loopback","value":"v"}}'
SETW='{"jsonrpc":"2.0","id":1,"method":"set","params":{"col":"0","key":"via-lan","value":"v"}}'
EXL='{"jsonrpc":"2.0","id":2,"method":"exists","params":{"col":"0","key":"via-lan"}}'

# ---- plaintext = loopback -------------------------------------------
conf "plaintext = loopback"
start
R=$(plain 127.0.0.1 18301 "$SETL")
echo "$R" | grep -q '"stored": *true' && ok "plaintext = loopback: a plaintext write on the loopback door is served" \
	|| bad "control failed: the loopback door did not serve a plaintext write under plaintext = loopback ($R)"
R=$(plain "$LANIP" 18302 "$SETW")
E=$(plain 127.0.0.1 18301 "$EXL")
echo "$R" | grep -q 'stored' && bad "the LAN door ($LANIP) SERVED a plaintext write: $R" \
	|| { echo "$E" | grep -q '"exists": *false' \
		&& ok "the same plaintext write on the LAN door ($LANIP) got no result ($R) and the key does not exist afterwards" \
		|| bad "the LAN door answered '$R' but the key's existence reads: $E"; }
R=$(enc "$LANIP" 18302 '{"method":"set","params":{"col":"0","key":"via-lan-enc","value":"v"}}')
echo "$R" | grep -q '"stored": *true' && ok "an encrypted client on that LAN door is served - the door is alive, what it refuses is plaintext" \
	|| bad "the encrypted control on the LAN door failed: $R"
stop

# ---- plaintext = never (the default) --------------------------------
conf ""
start
R=$(plain 127.0.0.1 18301 "$SETL")
echo "$R" | grep -q 'stored' && bad "plaintext = never: the loopback door SERVED a plaintext write: $R" \
	|| ok "plaintext = never (the default): the loopback door refuses plaintext too ($R)"
R=$(enc 127.0.0.1 18301 '{"method":"set","params":{"col":"0","key":"enc","value":"v"}}')
echo "$R" | grep -q '"stored": *true' && ok "and serves the encrypted client" \
	|| bad "plaintext = never: the encrypted client was not served: $R"
stop

echo "plaintextdoortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
