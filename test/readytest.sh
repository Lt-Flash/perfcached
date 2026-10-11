#!/bin/sh
# readytest.sh - "perfcached ready" means the doors accept.
#
# The workers open their own SO_REUSEPORT listeners after the main thread
# starts them, and the main thread logged "ready" without waiting: a client
# that connected on the line could be refused.  v0.5.6 GitLab check-asan
# (durability): waltest's client, started the moment the line appeared,
# got "Connection refused" on a loaded runner.  slowlistenshim.so holds every
# worker's listen() 300 ms, so the window is certain rather than a race:
# the native, RESP and HTTP doors are dialled the instant the line appears,
# three starts in a row, and none may refuse.
# Fail-first: f19c4b3 refuses every time under the shim.
# Usage: test/readytest.sh [./perfcached] [./slowlistenshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=$(readlink -f "${2:-./slowlistenshim.so}")
[ -f "$SHIM" ] || { echo "readytest: no $SHIM (make slowlistenshim.so)"; exit 1; }
D=$(mktemp -d /var/tmp/pcready.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18631 18632 18633; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "readytest: port $p busy" >&2; exit 1; }
done
cat > "$D/r.conf" <<C
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = ready-client-secret
[listen]
tcp = 127.0.0.1:18631
resp = 127.0.0.1:18632
http = 127.0.0.1:18633
plaintext = loopback
[collection 0]
buckets_log2 = 10
C
chmod 600 "$D/r.conf"
run=0
while [ $run -lt 3 ]; do
	run=$((run+1))
	LD_PRELOAD="$SHIM" PC_SLOWLISTEN_MS=300 "$BIN" -f "$D/r.conf" > "$D/log" 2>&1 &
	P=$!
	R=$(python3 - "$D/log" <<'PY'
import socket, sys, time
log = sys.argv[1]; t0 = time.time()
while time.time() - t0 < 30:
    try:
        if "perfcached ready" in open(log).read(): break
    except OSError: pass
    time.sleep(0.002)
else:
    print("no ready line in 30 s"); sys.exit()
out = []
for port in (18631, 18632, 18633):
    s = socket.socket()
    try:
        s.connect(("127.0.0.1", port)); out.append("%d accepted" % port)
    except OSError as e:
        out.append("%d %s" % (port, e.strerror))
    finally:
        s.close()
print(", ".join(out))
PY
)
	case "$R" in
	"18631 accepted, 18632 accepted, 18633 accepted") ok "start $run: all three doors accept on the ready line ($R)";;
	*) bad "start $run: on the ready line - $R";;
	esac
	kill $P 2>/dev/null; wait $P 2>/dev/null; P=
done
echo "readytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
