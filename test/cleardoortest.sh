#!/bin/sh
# cleardoortest.sh - S339: a native client with no secret is told so at
# once, and perfcli finds a config at /etc/perfcached.conf.
#
# PROD 2026-10-07: `perfcli -h <its own address>` on a node sat silent for
# ~31 s and then blamed a closed connection.  The client had no secret, so
# it sent its first request in the clear; the binary header's first two
# bytes (magic 0x9E, ver 1) read little-endian as a 414-byte Noise message
# 1 - inside the handshake's bounds - and the encrypted door waited for 416
# bytes that never came, until the CLIENT's 30 s io timeout.  It had no
# secret because the release-tarball install keeps its config at
# /etc/perfcached.conf, a path perfcli did not look at.
# plaintextdoortest.sh sends the same plaintext request and counts its 3 s
# TIMEOUT as "refused" - true, and blind to the wait.
#
# One daemon, `plaintext = loopback`, a native door on the host's LAN
# address and one on loopback:
#  - a plaintext request to the LAN door is CLOSED within a second (fails
#    first: TIMEOUT), and the daemon says why, naming the client;
#  - perfcli with no secret and no config fails within 5 s with the
#    no-secret message (fails first: ~30 s);
#  - perfcli with a config ONLY at /etc/perfcached.conf (fopenmapshim maps
#    that path to a file here - the host's /etc is not touched) reads it
#    and is answered with no -h/-a (fails first: it finds no config);
#  - control: perfcli -a <secret> on the LAN door is answered.
# Usage: test/cleardoortest.sh [./perfcached] [./perfcli] [./fopenmapshim.so]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
SHIM=${3:-./fopenmapshim.so}
D=$(mktemp -d /var/tmp/pccd.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
SEC=cd-client-secret
PORT=17846
LPORT=17847

LANIP=$(ip -4 -o addr show scope global 2>/dev/null \
	| awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$LANIP" ] || { echo "cleardoortest: no global IPv4 address on this host - SKIPPED, the off-loopback door cannot be built"; exit 0; }
for x in "$CLI" "$SHIM"; do
	[ -e "$x" ] || { echo "cleardoortest: $x is missing (make perfcli fopenmapshim.so)"; exit 1; }
done
SHIM=$(cd "$(dirname "$SHIM")" && pwd)/$(basename "$SHIM")
# perfcli takes the FIRST config it finds; one at an earlier path on this
# host would answer the /etc/perfcached.conf case for it
for c in /etc/perfcached/perfcached.conf /opt/perfcached/etc/perfcached.conf; do
	[ -e "$c" ] && { echo "cleardoortest: $c exists on this host and would be read first - SKIPPED"; exit 0; }
done

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
[listen]
tcp = $LANIP:$PORT
tcp = 127.0.0.1:$LPORT
plaintext = loopback
[collection 0]
buckets_log2 = 10
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

# 1. a plaintext request in the clear: closed at once, not left waiting
R=$(python3 - "$LANIP" "$PORT" <<'EOF'
import socket, sys, time
import pcnative
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=4)
s.settimeout(5)
t0 = time.time()
s.sendall(pcnative.frame(1, "ping", None))
try:
    d = s.recv(65536)
    print("%s %.2f" % ("CLOSED" if not d else "DATA(%d)" % len(d), time.time() - t0))
except socket.timeout:
    print("TIMEOUT %.2f" % (time.time() - t0))
except ConnectionResetError:
    print("CLOSED %.2f" % (time.time() - t0))
EOF
)
case "$R" in
CLOSED*) secs=${R#CLOSED }
	if awk -v s="$secs" 'BEGIN{exit !(s < 1.0)}'; then
		ok "a request in the clear to the encrypted door is closed at once ($R s)"
	else
		bad "a request in the clear is closed, but only after $secs s"
	fi;;
*) bad "a request in the clear to the encrypted door is left waiting ($R s) - the door read it as a handshake";;
esac
sleep 0.2
grep -q "sent a request in the clear to the native door" "$D/a.log" \
	&& ok "the daemon says why, naming the client ($(grep -m1 -o 'client [0-9.:]* sent a request in the clear' "$D/a.log"))" \
	|| bad "no line in the daemon's log says a client sent a request in the clear"

# 2. perfcli with no secret and no config: told within seconds
E="$D/empty"; mkdir -p "$E"
T0=$(date +%s)
OUT=$(env -u PERFCLI_AUTH -u PERFCACHED_CONF HOME="$E" timeout 20 "$CLI" -h "$LANIP" -p "$PORT" ping 2>&1); rc=$?
T=$(( $(date +%s) - T0 ))
if [ $rc -ne 0 ] && [ $T -le 5 ] && echo "$OUT" | grep -q "no client secret"; then
	ok "perfcli with no secret fails in ${T} s and says it has no secret"
else
	bad "perfcli with no secret: rc $rc after ${T} s (want <= 5 s and the no-secret message): $(echo "$OUT" | head -2)"
fi

# 3. a config only at /etc/perfcached.conf: perfcli reads it, no -h/-a
cat > "$D/etc.conf" <<CONF
[listen]
tcp = $LANIP:$PORT
[secrets]
client = $SEC
CONF
OUT=$(env -u PERFCLI_AUTH -u PERFCACHED_CONF HOME="$E" LD_PRELOAD="$SHIM" \
	PC_FOPEN_FROM=/etc/perfcached.conf PC_FOPEN_TO="$D/etc.conf" \
	timeout 10 "$CLI" ping 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$OUT" | grep -q -i "pong"; then
	ok "perfcli reads /etc/perfcached.conf and is answered with no -h or -a ($(echo "$OUT" | head -1))"
else
	bad "perfcli with a config only at /etc/perfcached.conf: rc $rc: $(echo "$OUT" | head -2)"
fi

# 4. control: the door serves a client that has the secret
OUT=$(env -u PERFCACHED_CONF HOME="$E" timeout 10 "$CLI" -h "$LANIP" -p "$PORT" -a "$SEC" ping 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$OUT" | grep -q -i "pong" \
	&& ok "control: the LAN door answers a client with the secret" \
	|| bad "control: the LAN door did not answer a client with the secret (rc $rc: $(echo "$OUT" | head -1))"

echo "cleardoortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
