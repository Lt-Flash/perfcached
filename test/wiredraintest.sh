#!/bin/sh
# wiredraintest.sh - a connection's output limit is on its BACKLOG, not
# on its traffic.
#
# The wire buffer is reset only when it drains completely, and the limit
# (PC_MAX_OUTQ + 1/16) was checked against everything appended since that
# reset - bytes already in the socket included.  A reader that keeps up
# but never quite empties the buffer was closed after 8.5 MB of TRAFFIC,
# as a "write error" no counter records: GitHub rc46 check-standalone,
# pushbatchtest's burst arm, one subscriber cut at 1,931 of 4,000 4 KB
# frames with dropped 0 and slow kills 0.
#
# pushbatchtest caught it by luck - a fast reader drains between flushes.
# Here the reader is FLOW-CONTROLLED to lag the publisher by 5-6 MB:
# more than loopback's socket buffers hold, so the server always has
# something pending, and less than the 8 MB slow-consumer cap, so a
# close is never the legitimate one.  20 MB crosses the old limit twice
# over, however fast or slow the host.
#   - every frame arrives, in order, and the connection is not closed
#   - no slow kill, nothing dropped (the lag stayed under the cap)
#   - the backlog was really the SERVER's: /clients shows the subscriber
#     pending >= 512 KB (a lag the sockets absorb proves nothing - the
#     first cut of this test passed on the unfixed build for exactly
#     that reason: 5-6 MB fit in loopback's buffers)
# Fail-first: the build before the fix cuts the subscriber at ~8.5 MB.
# Usage: test/wiredraintest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwiredrain.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/w.conf" <<CONF
[daemon]
workers = 1
[memory]
arena_mb = 32
[secrets]
client = wiredrain-client-secret
[listen]
tcp = 127.0.0.1:17581
resp = 127.0.0.1:17582
http = 127.0.0.1:17583
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/w.conf"
"$BIN" -f "$D/w.conf" -D > "$D/w.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/w.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/w.log"; then
	bad "the daemon did not start: $(tail -1 "$D/w.log")"
	echo "wiredraintest: $pass passed, $fail failed"; exit 1
fi

RESFILE="$D/w.res" timeout 120 python3 - <<'PY' 2> "$D/w.py.err"
import json, os, socket, threading, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def stats():
    return json.load(urllib.request.urlopen("http://127.0.0.1:17583/stats", timeout=5))["pubsub"]

PL, NB = 4096, 5000                       # 5,000 x ~4.1 KB = ~20 MB
LAG_LO, LAG_HI = 5 << 20, 6 << 20
hdr = b"*3\r\n$7\r\nmessage\r\n$2\r\nst\r\n$%d\r\n" % PL
fl = len(hdr) + PL + 2

# a small receive window, so the lag cannot hide in the client's socket:
# what the server's send buffer cannot hold (tcp_wmem max, 4 MB here)
# stays in its OWN wire buffer - the backlog under test
sub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 << 10)
sub.connect(("127.0.0.1", 17582))
sub.settimeout(30)
sub.sendall(b"*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nst\r\n")
buf = b""
while buf.count(b"\r\n") < 6:
    buf += sub.recv(4096)
buf = buf.split(b"\r\n", 6)[6]
pub = socket.create_connection(("127.0.0.1", 17582))
pub.settimeout(30)

st0 = stats()
lock = threading.Condition()
state = {"sent": 0, "seen": 0, "done": False, "waits_at_lag": 0, "err": None, "good": True,
         "samples": 0, "pend_max": 0, "pend_pos": 0}

def sampler():
    # the server's own figure: bytes the subscriber's socket would not take
    while not state["done"] or state["seen"] < state["sent"]:
        if state["err"] is not None:
            break
        try:
            rows = json.load(urllib.request.urlopen("http://127.0.0.1:17583/clients", timeout=5))
            rows = rows.get("clients", rows) if isinstance(rows, dict) else rows
            p = max((r.get("pending", 0) for r in rows if r.get("subs", 0) > 0), default=0)
        except Exception:
            p = 0
        state["samples"] += 1
        state["pend_max"] = max(state["pend_max"], p)
        state["pend_pos"] += p > 0
        time.sleep(0.02)

def publisher():
    for i in range(NB):
        with lock:
            # never more than LAG_HI ahead of what the reader has taken
            while (i - state["seen"]) * fl >= LAG_HI and state["err"] is None:
                lock.wait(0.05)
            if state["err"] is not None:
                break
        pub.sendall(b"*3\r\n$7\r\nPUBLISH\r\n$2\r\nst\r\n$%d\r\n%08d%s\r\n" % (PL, i, b"x" * (PL - 8)))
        with lock:
            state["sent"] = i + 1
            lock.notify_all()
    with lock:
        state["done"] = True
        lock.notify_all()

def reader():
    b, seen = buf, 0
    try:
        while seen < NB:
            with lock:
                # read only while at least LAG_LO is outstanding - so the
                # server always holds a backlog - or once publishing ended
                while not state["done"] and (state["sent"] - seen) * fl < LAG_LO:
                    lock.wait(0.05)
                if (state["sent"] - seen) * fl >= LAG_LO:
                    state["waits_at_lag"] += 1
            while len(b) < fl:
                d = sub.recv(1 << 16)
                if not d:
                    raise EOFError("closed by the server")
                b += d
            n = len(b) // fl
            for j in range(n):
                f = b[j * fl:(j + 1) * fl]
                if not f.startswith(hdr) or f[len(hdr):len(hdr) + 8] != b"%08d" % seen:
                    state["good"] = False
                seen += 1
            b = b[n * fl:]
            with lock:
                state["seen"] = seen
                lock.notify_all()
    except Exception as e:
        with lock:
            state["err"] = "%s: %s" % (type(e).__name__, e)
            state["seen"] = seen
            lock.notify_all()

tp, tr = threading.Thread(target=publisher), threading.Thread(target=reader)
ts = threading.Thread(target=sampler)
tr.start(); tp.start(); ts.start()
tp.join(90); tr.join(90); ts.join(10)
replies = b""
pub.settimeout(5)
try:
    while replies.count(b"\r\n") < state["sent"]:
        d = pub.recv(65536)
        if not d:
            break
        replies += d
except socket.timeout:
    pass
time.sleep(0.3)
st1 = stats()
d = lambda k: st1[k] - st0[k]
check(state["err"] is None and state["seen"] == NB and state["good"],
      "a subscriber lagging 5-6 MB behind receives all %d frames (%d MB), in order (seen %d, error %s)"
      % (NB, NB * fl >> 20, state["seen"], state["err"]))
check(d("slow_kills") == 0 and d("queue_dropped") == 0,
      "the lag stayed under the cap: no slow kill, nothing dropped (slow %d, dropped %d)"
      % (d("slow_kills"), d("queue_dropped")))
check(state["waits_at_lag"] > 0 and state["pend_max"] >= (512 << 10),
      "the SERVER held a backlog: pending up to %d KB, > 0 in %d of %d /clients samples (reader lagged >= 5 MB %d times)"
      % (state["pend_max"] >> 10, state["pend_pos"], state["samples"], state["waits_at_lag"]))
check(replies.count(b"\r\n") == state["sent"] and all(l == b":1" for l in replies.split(b"\r\n")[:state["sent"]]),
      "every PUBLISH answered 1 receiver (%d of %d)" % (replies.count(b"\r\n"), state["sent"]))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$D/w.res"
[ -s "$D/w.res" ] || bad "the python driver produced nothing"
[ -s "$D/w.py.err" ] && { bad "the python driver raised:"; tail -3 "$D/w.py.err"; }
grep -q "write error" "$D/w.log" && bad "the daemon logged a write-error close: $(grep "write error" "$D/w.log" | tail -1)"
kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
echo "wiredraintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
