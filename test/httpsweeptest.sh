#!/bin/sh
# httpsweeptest.sh - the metrics-door sweep (after epoll_wait, before dispatch) must not free a
# connection that still has an event in the batch: clients timed across tick boundaries, no crash;
# an epoll worker whose wait ends in EINTR with a refused peer parked keeps serving.
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pchttpsweep.XXXXXX); P1= P2=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; [ -n "$P2" ] && kill -9 "$P2" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 1
[memory]
arena_mb = 32
[secrets]
client = httpsweep-client-secret
[listen]
tcp = 127.0.0.1:17597
http = 127.0.0.1:18597
plaintext = loopback
http_timeout = 1
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"; echo "httpsweeptest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 60 python3 - <<'PY' 2> "$D/py.err"
import os, random, socket, threading, time
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
REQ = b"GET /stats HTTP/1.0\r\nHost: x\r\n\r\n"

def get():
    """one request; (True, reply) answered, (False, why) otherwise"""
    s = socket.create_connection(("127.0.0.1", 18597), timeout=5)
    try:
        s.sendall(REQ)
        reply = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            reply += d
        return reply.startswith(b"HTTP/1.") and b" 200 " in reply.split(b"\r\n", 1)[0], reply
    finally:
        s.close()

ok0, _ = get()
check(ok0, "the http door answers a request (positive control)")

lock = threading.Lock()
cnt = {"answered": 0, "swept_after_send": 0, "closed_by_client": 0, "refused": 0}
stop = time.time() + 8

def client(seed):
    rnd = random.Random(seed)
    while time.time() < stop:
        try:
            s = socket.create_connection(("127.0.0.1", 18597), timeout=5)
        except OSError:
            with lock:
                cnt["refused"] += 1
            time.sleep(0.2)
            continue
        time.sleep(rnd.uniform(0.15, 0.95))
        if rnd.random() < 0.25:
            s.close()                    # a bare FIN is an event too
            with lock:
                cnt["closed_by_client"] += 1
            continue
        try:
            s.sendall(REQ)
            reply = b""
            while True:
                d = s.recv(65536)
                if not d:
                    break
                reply += d
            k = "answered" if reply.startswith(b"HTTP/1.") else "swept_after_send"
        except OSError:
            k = "swept_after_send"
        finally:
            s.close()
        with lock:
            cnt[k] += 1

ts = [threading.Thread(target=client, args=(n,), daemon=True) for n in range(3)]
for t in ts:
    t.start()
for t in ts:
    t.join(20)
res.write("I race: %(answered)d answered, %(swept_after_send)d swept with the request sent, "
          "%(closed_by_client)d closed by the client, %(refused)d refused\n" % cnt)
check(cnt["answered"] > 0, "requests were answered during the race (%d)" % cnt["answered"])
check(cnt["swept_after_send"] > 0,
      "the sweep raced requests already sent - the window was exercised (%d)" % cnt["swept_after_send"])
check(cnt["refused"] == 0, "no connection was refused during the race (%d)" % cnt["refused"])
try:
    ok1, _ = get()
except OSError as e:
    ok1 = False
check(ok1, "the http door still answers after the race")
PY
drv=$?; [ $drv -eq 0 ] || echo "F the driver stopped: exit $drv" >> "$RES"   # S488
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; I\ *) echo "  ..   ${l#I }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
if kill -0 "$P1" 2>/dev/null; then ok "the daemon is still running"; else bad "the daemon died: $(grep -a -m1 -E 'AddressSanitizer|runtime error|SEGV|egfault' "$D/n.log" | cut -c1-160)"; fi
if grep -a -q -E "AddressSanitizer|runtime error:|SEGV|egfault" "$D/n.log"; then
	bad "sanitizer report or segfault in the log: $(grep -a -m1 -E 'AddressSanitizer|runtime error:|SEGV|egfault' "$D/n.log" | cut -c1-160)"
else
	ok "no sanitizer report or segfault in the log"
fi
# ---- 2. an epoll worker interrupted while a refused HTTP peer is parked ------------------------
# the sweeps after epoll_wait make system calls (a parked 403 peer's recv sets EAGAIN): a stop and
# continue that ends the wait with EINTR must not end the worker - its door must still answer
cat > "$D/e.conf" <<CONF
[daemon]
workers = 1
io = epoll
[memory]
arena_mb = 32
[secrets]
client = httpsweep-client-secret
[listen]
tcp = 127.0.0.1:17598
http = 127.0.0.1:18598
http_allow = 127.0.0.2/32
plaintext = loopback
[collection 0]
buckets_log2 = 10
CONF
chmod 640 "$D/e.conf"
"$BIN" -f "$D/e.conf" > "$D/e.log" 2>&1 & P2=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/e.log" && break; sleep 0.1; i=$((i+1)); done
if grep -q "perfcached ready" "$D/e.log"; then
	: > "$RES"
	RESFILE="$RES" PID2=$P2 timeout 60 python3 - <<'PY' 2> "$D/py2.err"
import os, signal, socket, time
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
pid = int(os.environ["PID2"])
REQ = b"GET /stats HTTP/1.0\r\nHost: x\r\n\r\n"

def get():
    """a request from 127.0.0.2, the one address http_allow admits"""
    try:
        s = socket.socket()
        s.settimeout(3)
        s.bind(("127.0.0.2", 0))
        s.connect(("127.0.0.1", 18598))
        s.sendall(REQ)
        reply = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            reply += d
        s.close()
        return reply.split(b"\r\n", 1)[0].endswith(b" 200 OK")
    except OSError:
        return False

check(get(), "2. epoll, http_allow 127.0.0.2: the door answers it (positive control)")
denied = 0
for n in range(3):
    r = socket.create_connection(("127.0.0.1", 18598), timeout=3)   # refused: parked 1 s, held open
    denied += r.recv(4096).startswith(b"HTTP/1.0 403")
    os.kill(pid, signal.SIGSTOP)
    time.sleep(0.2)
    os.kill(pid, signal.SIGCONT)
    time.sleep(0.3)
    r.close()
check(denied == 3, "2. three peers from 127.0.0.1 answered 403 and parked (%d)" % denied)
check(get(), "2. after three stop/continue rounds with a refused peer parked, the door still answers")
PY
	drv=$?; [ $drv -eq 0 ] || echo "F 2. the driver stopped: exit $drv" >> "$RES"
	while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
	[ -s "$D/py2.err" ] && { bad "2. the python driver raised:"; tail -3 "$D/py2.err"; }
	kill "$P2" 2>/dev/null; wait "$P2" 2>/dev/null; P2=
else
	bad "2. the epoll daemon did not start: $(tail -2 "$D/e.log" | tr '\n' ' ')"
fi
echo "httpsweeptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
