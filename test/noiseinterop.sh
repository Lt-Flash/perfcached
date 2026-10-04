#!/bin/sh
# noiseinterop.sh — S25' interop proof: an independent Python NNpsk0
# initiator (test/noise_interop.py) handshakes with the live daemon over
# an encryption-required listener, then exchanges real requests.  Also
# proves wrong-PSK and tampered-handshake connections are rejected, and
# that a plaintext client on the encrypted port gets nothing.
# Usage: test/noiseinterop.sh [./perfcached] [./noisetest]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
NT=${2:-./noisetest}
HERE=$(dirname "$0")
D=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT

PW="interop-test-password"
PSK=$("$NT" psk client "$PW") || { echo "FAIL: psk derive"; exit 1; }

cat > "$D/enc.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = $PW
cluster = a-distinct-cluster-secret
[listen]
tcp = 127.0.0.1:16481
plaintext = never
[collection th]
buckets_log2 = 12
EOF

"$BIN" -f "$D/enc.conf" > "$D/log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || { echo "FAIL: daemon died"; cat "$D/log"; exit 1; }
	sleep 0.1; i=$((i+1))
done
grep -q "Noise PSKs derived" "$D/log" || { echo "FAIL: PSKs not derived"; cat "$D/log"; exit 1; }

fails=0
run() { # run <mode> <label>
	python3 "$HERE/noise_interop.py" 127.0.0.1 16481 0 "$PSK" "$1"
	rc=$?
	if [ $rc = 77 ]; then
		# the host's python cryptography cannot drive the handshake -
		# nothing here is testable, and a FAIL would say the daemon
		# refused a valid client when no client was ever built
		echo "  ..   SKIP: this host's python cryptography is too old to "\
			"drive the interop initiator"
		echo "noiseinterop: SKIPPED"
		exit 0
	fi
	if [ $rc = 0 ]; then
		:
	else
		echo "FAIL: interop $2"; fails=$((fails+1))
	fi
}
run ok       "handshake + requests"
run wrongpsk "wrong-PSK rejection"
run tamper   "tampered-handshake rejection"

# a plaintext client on the encrypted port must get nothing back.  The
# probe is a native CMD frame (test/pcnative.py).  The door reads its
# first two bytes (0x9E 0x01) as a handshake length of 414, so the frame
# carries enough to fill that bogus handshake - the daemon then refuses
# it and closes, where a shorter frame would leave it waiting for more.
if python3 - <<'EOF'
import pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", 16481), timeout=3)
s.sendall(pcnative.frame(1, "ping", {"echo": "x" * 512}))
sys.exit(0 if s.recv(64) == b"" else 1)
EOF
then :; else echo "FAIL: plaintext-on-encrypted got a reply"; fails=$((fails+1)); fi

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "noiseinterop: $([ $fails -eq 0 ] && echo PASS || echo "$fails FAILED")"
[ $fails -eq 0 ]
