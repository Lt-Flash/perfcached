#!/bin/sh
# httpidletest.sh - S46's idle HTTP timeout on a quiet worker.  A metrics
# connection that never sends its request is closed after `http_timeout`
# seconds - but the worker only sweeps when it wakes, and it chose how
# long to wait from a count taken BEFORE that turn's accepts.  A new idle
# HTTP connection on an otherwise quiet worker was therefore never
# counted, the worker waited with no timeout, and nothing closed it.
#   - one worker, so every connection shares its wait
#   - an idle native connection opened first (native doors are never
#     swept; it must survive, and opening it later would wake the worker)
#   - a finished HTTP request first, as the positive control
#   - then an HTTP connection that sends nothing: closed by the daemon
#     within ~4 s of a 2 s timeout
# Fail-first: the build before the live count keeps it open.
# Usage: test/httpidletest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pchttpidle.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 1
[memory]
arena_mb = 32
[secrets]
client = httpidle-client-secret
[listen]
tcp = 127.0.0.1:17531
http = 127.0.0.1:18531
plaintext = loopback
http_timeout = 2
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; echo "httpidletest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 60 python3 - <<'PY' 2> "$D/py.err"
import os, select, socket, time
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def closed_within(s, secs):
    end = time.time() + secs
    while time.time() < end:
        if select.select([s], [], [], max(0, end - time.time()))[0]:
            try:
                if s.recv(4096) == b"":
                    return True
            except ConnectionResetError:
                return True
    return False

native = socket.create_connection(("127.0.0.1", 17531))
h = socket.create_connection(("127.0.0.1", 18531))
h.sendall(b"GET /stats HTTP/1.0\r\nHost: x\r\n\r\n")
reply = b""
while True:
    d = h.recv(65536)
    if not d: break
    reply += d
check(reply.startswith(b"HTTP/1.") and b" 200 " in reply.split(b"\r\n", 1)[0], "the http door answers a request (positive control)")
time.sleep(2.5)                       # the worker settles into its wait
idle = socket.create_connection(("127.0.0.1", 18531))
t0 = time.time()
gone = closed_within(idle, 6)
dt = time.time() - t0
check(gone and dt <= 4.0, "an HTTP connection that sends nothing is closed within 4 s of a 2 s timeout on a quiet worker (%s after %.1f s)" % ("closed" if gone else "still open", dt))
check(not select.select([native], [], [], 0)[0], "the idle native connection is not swept")
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
echo "httpidletest: $pass passed, $fail failed"
[ $fail -eq 0 ]
