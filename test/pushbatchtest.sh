#!/bin/sh
# pushbatchtest.sh - a worker writes a subscriber's pub/sub pushes once
# per turn, not once per message.  A publish staged its frame and wrote
# it at once, so every delivered message cost the worker a write(2):
# measured against Redis 8 on one core, 350,064 deliveries took 467,871
# writes and a worker's delivery ceiling was ~120k messages/s where
# Redis did 580k.  The frames are now staged and each connection is
# flushed once when the worker's turn ends (sooner past a high-water
# mark, so a burst never reaches the slow-consumer cap on output the
# socket would take), and a worker's wakes to other workers wait for the
# end of its turn too, so the target drains a turn's messages as one
# batch instead of waking for each.
#   - the workers' output operations across a pipelined burst of 2000
#     PUBLISH, against the deliveries made: /metrics
#     perfcached_io_worker_ops_total{op="out"}, a worker's write() calls
#     or io_uring sends (S361 - an io_uring worker makes no write(), so
#     /proc's syscw, the counter this test first read, cannot see it)
#   - one worker, one RESP subscriber: the publisher's own turn
#   - two workers, four subscribers, and proof from the per-worker
#     counters that two workers wrote: the cross-worker drain
#   - every message arrives, in order, and PUBLISH answers the receivers
#   - a pipelined burst past the queue's 16 MB cap, to subscribers on
#     another worker: the queue drops nothing, a subscriber there (read by
#     head -c, so the host's speed does not decide it) receives it all in
#     order, one subscriber slow on purpose (a 64 KB receive buffer,
#     nothing read for 3 s; 16 MB each is twice a connection's 8 MB cap) is
#     closed by the cap, and every one that stopped short was a COUNTED
#     slow kill.  A wake that waited for
#     the end of the publisher's turn let one turn fill the cap first
#     (9,950 of 12,000 4 KB publishes dropped; here 1,981 of 4,000); a turn wakes the target
#     at once when it has queued a drain batch or 256 KB.
# Fail-first: the build that flushes per push makes one write a delivery
# (2001 and 8021 writes); staging without the deferred wakes still made
# 1164-1795 in the cross-worker case.
# Usage: test/pushbatchtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpushbatch.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

run_case() { # run_case <name> <workers> <subscribers>
	cat > "$D/$1.conf" <<CONF
[daemon]
workers = $2
[memory]
arena_mb = 32
[secrets]
client = pushbatch-client-secret
[listen]
tcp = 127.0.0.1:17561
resp = 127.0.0.1:17562
http = 127.0.0.1:17563
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/$1.conf"
	"$BIN" -f "$D/$1.conf" -D > "$D/$1.log" 2>&1 & P1=$!
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/$1.log" && break; sleep 0.1; i=$((i+1)); done
	if ! grep -q "perfcached ready" "$D/$1.log"; then
		bad "$1: the daemon did not start: $(tail -1 "$D/$1.log")"
		kill -9 "$P1" 2>/dev/null; P1=; return
	fi
	: > "$D/$1.res"
	RESFILE="$D/$1.res" PID="$P1" NSUB=$3 NAME=$1 timeout 60 python3 - <<'PY' 2> "$D/$1.py.err"
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
name, nsub, pid = os.environ["NAME"], int(os.environ["NSUB"]), os.environ["PID"]
N = 2000
def check(c, m): res.write(("P " if c else "F ") + name + ": " + m + "\n"); res.flush()
def worker_out():                                # worker -> output ops (S361)
    out = {}
    for l in urllib.request.urlopen("http://127.0.0.1:17563/metrics", timeout=5).read().decode().splitlines():
        if l.startswith("perfcached_io_worker_ops_total{") and 'op="out"' in l:
            out["pc-w" + l.split('worker="')[1].split('"')[0]] = int(l.split()[-1])
    return out
def syscw():                                      # all the workers' output ops
    w = worker_out()
    return sum(w.values()) if w else None
def stats():
    return json.load(urllib.request.urlopen("http://127.0.0.1:17563/stats", timeout=5))["pubsub"]

def recv_until(sock, buf, n):
    while len(buf) < n:
        d = sock.recv(65536)
        if not d:
            break
        buf += d
    return buf
def thread_writes():                              # worker -> output ops
    return worker_out()
def connect():
    subs = []
    for i in range(nsub):
        s = socket.create_connection(("127.0.0.1", 17562))
        s.settimeout(20)
        s.sendall(b"*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n")
        subs.append([s, b""])
    for sb in subs:                               # the subscribe confirmation
        while sb[1].count(b"\r\n") < 6:
            sb[1] += sb[0].recv(4096)
        sb[1] = sb[1].split(b"\r\n", 6)[6]
    pub = socket.create_connection(("127.0.0.1", 17562))
    pub.settimeout(20)
    return subs, pub

# The cross-worker drain is the case the wakes matter to, and where a
# connection lands is the kernel's choice: publish one message and see
# which workers wrote - the publisher's for the reply, another only if a
# subscriber sits there - and reconnect until two did.
warm = b"*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$4\r\nwarm\r\n"
for attempt in range(12):
    subs, pub = connect()
    before = thread_writes()
    pub.sendall(b"*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$4\r\nwarm\r\n")
    recv_until(pub, b"", 4)
    for sb in subs:
        sb[1] = recv_until(sb[0], sb[1], len(warm))[len(warm):]
    time.sleep(0.2)
    after = thread_writes()
    wrote = sorted(k for k in after if after[k] > before.get(k, 0))
    if nsub == 1 or len(wrote) >= 2:
        break
    for sb in subs:
        sb[0].close()
    pub.close()
    time.sleep(0.2)
check(stats()["subscribers"] == nsub, "%d subscriptions registered" % nsub)
if nsub > 1:
    check(len(wrote) >= 2, "the subscribers span workers, so the cross-worker drain runs (workers that wrote: %s, attempt %d)" % (",".join(wrote), attempt + 1))

st0 = stats()
w0 = syscw()
check(w0 is not None and w0 > 0, "the workers' output counters are readable (%r)" % w0)
burst = b"".join(b"*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$8\r\n%08d\r\n" % i for i in range(N))
pub.sendall(burst)
replies = b""
end = time.time() + 20
while replies.count(b"\r\n") < N and time.time() < end:
    replies += pub.recv(65536)
lines = replies.split(b"\r\n")[:N]
check(len(lines) == N and all(l == b":%d" % nsub for l in lines),
      "every PUBLISH answers %d receiver(s) (%d replies)" % (nsub, len(lines)))

frame = lambda i: b"*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$8\r\n%08d\r\n" % i
flen = flen0 = len(frame(0))
for sb in subs:
    sb[1] = recv_until(sb[0], sb[1], N * flen)
got_all = all(sb[1] == b"".join(frame(i) for i in range(N)) for sb in subs)
check(got_all, "every subscriber received all %d messages, in order" % N)
time.sleep(0.3)
w1 = syscw()
st = stats()
deliveries = N * nsub
writes = (w1 - w0) if (w0 is not None and w1 is not None) else -1
delta = lambda k: st[k] - st0[k]
check(delta("delivered") == deliveries and delta("queue_dropped") == 0 and delta("slow_kills") == 0,
      "the engine counts %d delivered, none dropped (%d, dropped %d, slow %d)"
      % (deliveries, delta("delivered"), delta("queue_dropped"), delta("slow_kills")))
check(0 <= writes <= deliveries // 8,
      "%d deliveries (and %d replies) took %d writes or sends - at most one per 8 deliveries"
      % (deliveries, N, writes))

if nsub > 1:
    import threading
    NB, PL = 4000, 4096
    hdr = b"*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$%d\r\n" % PL
    fl = len(hdr) + PL + 2
    results = [(0, False)] * nsub
    errs = [None] * nsub
    # Which subscribers sit on the worker that is NOT the publisher's: the
    # ones the burst reaches through the cross-worker queue.  /clients does
    # not say, so ask the threads, as the warm-up does: each subscriber also
    # joins a channel of its own, one message goes there, and two workers
    # writing (the reply, and the delivery) means the subscriber is on the
    # other one.
    other = [False] * nsub
    for k, sb in enumerate(subs):
        own = b"own%d" % k
        sock, pre = sb[0], sb[1][N * flen0:]
        sock.sendall(b"*2\r\n$9\r\nSUBSCRIBE\r\n$%d\r\n%s\r\n" % (len(own), own))
        conf = b"*3\r\n$9\r\nsubscribe\r\n$%d\r\n%s\r\n:2\r\n" % (len(own), own)
        pre = recv_until(sock, pre, len(conf))
        before = thread_writes()
        pub.sendall(b"*3\r\n$7\r\nPUBLISH\r\n$%d\r\n%s\r\n$2\r\nid\r\n" % (len(own), own))
        recv_until(pub, b"", 4)
        msg = b"*3\r\n$7\r\nmessage\r\n$%d\r\n%s\r\n$2\r\nid\r\n" % (len(own), own)
        pre = recv_until(sock, pre, len(conf) + len(msg))
        time.sleep(0.2)
        after = thread_writes()
        other[k] = sum(1 for t in after if after[t] > before.get(t, 0)) >= 2
        check(pre[:len(conf) + len(msg)] == conf + msg,
              "subscriber %d: its own channel answers (%s worker)" % (k, "the other" if other[k] else "the publisher's"))
        sb[1] = pre[len(conf) + len(msg):]
    # 0.6.1: the readers no longer decide the verdict.  Whether a Python
    # reader kept up with 16 MB was the HOST's: on GitHub's 2-core runner
    # all three readers on the other worker fell 8 MB behind and were closed
    # (counted, as they should be) - and no subscriber there was left to
    # show the queue delivers the whole burst.  So the fast ones read
    # through head -c (C speed; the order is checked on what arrived), and
    # one is slow ON PURPOSE - a 64 KB receive buffer, nothing read for 3 s
    # - so the output cap closes it, counted, on every host and not only on
    # a slow one (the io_uring send-in-flight path of fc4f301 included).
    # The daemon asks for a 1 MB send buffer (2 MB granted): ~2 MB fits in
    # the kernel, ~14 MB backs up - past the 8 MB cap.
    import shutil, subprocess, tempfile
    ones = [k for k in range(nsub) if other[k]]
    slow_k = ones[-1] if len(ones) >= 2 else next(k for k in range(nsub) if not other[k])
    subs[slow_k][0].setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    tdir = tempfile.mkdtemp(prefix="pcpbt.", dir="/var/tmp")
    def verify(k, data):
        seen, good = 0, True
        for j in range(len(data) // fl):
            f = data[j * fl:(j + 1) * fl]
            if not f.startswith(hdr) or f[len(hdr):len(hdr) + 8] != b"%08d" % j:
                good = False
            seen += 1
        results[k] = (seen, good)             # good = in order, as far as it got
    def reader(k, sock, pre):                 # the slow one: late, then Python
        buf = pre
        time.sleep(3)
        sock.settimeout(10)
        try:
            while len(buf) < NB * fl:
                d = sock.recv(1 << 20)
                if not d:
                    raise EOFError
                buf += d
        except Exception as e:
            errs[k] = type(e).__name__        # EOF = the server closed it
        verify(k, buf)
    def fast(k, sock, pre):                   # head -c reads the socket
        total = NB * fl
        path = os.path.join(tdir, "s%d" % k)
        sock.setblocking(True)
        with open(path, "wb") as out:
            p = subprocess.Popen(["head", "-c", str(max(0, total - len(pre)))], stdin=sock.fileno(), stdout=out)
            try:
                p.wait(timeout=40)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait(); errs[k] = "timeout"
        with open(path, "rb") as f:
            data = pre + f.read()
        if len(data) < total and errs[k] is None:
            errs[k] = "EOFError"              # head met the end: the server closed it
        verify(k, data)
    st1 = stats()
    th = [threading.Thread(target=reader if k == slow_k else fast, args=(k, sb[0], sb[1])) for k, sb in enumerate(subs)]
    for t in th:
        t.start()
    pub.sendall(b"".join(b"*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$%d\r\n%08d%s\r\n" % (PL, i, b"x" * (PL - 8)) for i in range(NB)))
    replies = b""
    end = time.time() + 30
    while replies.count(b"\r\n") < NB and time.time() < end:
        replies += pub.recv(65536)
    for t in th:
        t.join(30)
    time.sleep(0.3)
    st2 = stats()
    d2 = lambda k: st2[k] - st1[k]
    whole = [results[k] == (NB, True) and errs[k] is None for k in range(nsub)]
    short = [k for k in range(nsub) if not whole[k]]
    # The burst is 16 MB to each subscriber, twice the 8 MB a connection
    # may have waiting: whether a Python reader keeps up is the HOST's
    # (rc48, a 2-core runner: one reader 8 MB behind, closed, counted).
    # What this arm exists for is the queue between the workers.
    check(d2("queue_dropped") == 0,
          "a %d MB pipelined burst: the cross-worker queue dropped nothing (%d)"
          % (NB * PL >> 20, d2("queue_dropped")))
    # A reader that stopped short must be the output cap doing its job -
    # closed, in order up to the cut, and COUNTED.  One closed with no
    # slow kill is rc46's silent close (see wiredraintest).
    check(all(errs[k] == "EOFError" and results[k][1] for k in short) and len(short) == d2("slow_kills"),
          "every subscriber that stopped short was closed by a counted slow kill (short %s, slow kills %d, received %s, errors %s)"
          % (short, d2("slow_kills"), [r[0] for r in results], errs))
    check(any(whole[k] and other[k] for k in range(nsub)),
          "a subscriber on the other worker received the whole burst in order, through the queue (other %s, whole %s)"
          % (other, whole))
    check(slow_k in short and errs[slow_k] == "EOFError",
          "the subscriber slow on purpose (%d, on %s worker: a 64 KB receive buffer, nothing read for 3 s) was closed by the output cap (received %d of %d)"
          % (slow_k, "the other" if other[slow_k] else "the publisher's", results[slow_k][0], NB))
    shutil.rmtree(tdir, ignore_errors=True)
PY
	while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$D/$1.res"
	[ -s "$D/$1.res" ] || bad "$1: the python driver produced nothing"
	[ -s "$D/$1.py.err" ] && { bad "$1: the python driver raised:"; tail -3 "$D/$1.py.err"; }
	kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
}

run_case one-worker 1 1
run_case two-workers 2 4
echo "pushbatchtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
