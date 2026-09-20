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
#   - the daemon's own write-syscall counter (/proc/PID/io syscw) across
#     a pipelined burst of 2000 PUBLISH, against the deliveries it made
#   - one worker, one RESP subscriber: the publisher's own turn
#   - two workers, four subscribers, and proof from the per-thread
#     counters that two workers wrote: the cross-worker drain
#   - every message arrives, in order, and PUBLISH answers the receivers
#   - a pipelined burst past the queue's 16 MB cap, to subscribers that
#     keep up on another worker: nothing dropped.  A wake that waited for
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
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
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
def syscw():
    for l in open("/proc/%s/io" % pid):
        if l.startswith("syscw:"):
            return int(l.split()[1])
    return None
def stats():
    return json.load(urllib.request.urlopen("http://127.0.0.1:17563/stats", timeout=5))["pubsub"]

def recv_until(sock, buf, n):
    while len(buf) < n:
        d = sock.recv(65536)
        if not d:
            break
        buf += d
    return buf
def thread_writes():                              # worker thread -> syscw
    out = {}
    for t in os.listdir("/proc/%s/task" % pid):
        try:
            comm = open("/proc/%s/task/%s/comm" % (pid, t)).read().strip()
            if comm.startswith("pc-w"):
                for l in open("/proc/%s/task/%s/io" % (pid, t)):
                    if l.startswith("syscw:"):
                        out[comm] = int(l.split()[1])
        except OSError:
            pass
    return out
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
check(w0 is not None and w0 > 0, "the daemon's write-syscall counter is readable (%r)" % w0)
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
      "%d deliveries (and %d replies) took %d writes - at most one per 8 deliveries"
      % (deliveries, N, writes))

if nsub > 1:
    import threading
    NB, PL = 4000, 4096
    hdr = b"*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$%d\r\n" % PL
    fl = len(hdr) + PL + 2
    results = [(0, False)] * nsub
    def reader(k, sock, pre):
        buf, seen, good = pre, 0, True
        sock.settimeout(10)
        try:
            while seen < NB:
                while len(buf) < fl:
                    d = sock.recv(1 << 20)
                    if not d:
                        raise EOFError
                    buf += d
                n = len(buf) // fl
                for j in range(n):
                    f = buf[j * fl:(j + 1) * fl]
                    if not f.startswith(hdr) or f[len(hdr):len(hdr) + 8] != b"%08d" % seen:
                        good = False
                    seen += 1
                buf = buf[n * fl:]
        except Exception:
            good = False
        results[k] = (seen, good)
    st1 = stats()
    th = [threading.Thread(target=reader, args=(k, sb[0], sb[1][N * flen0:])) for k, sb in enumerate(subs)]
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
    check(all(r == (NB, True) for r in results) and d2("queue_dropped") == 0 and d2("slow_kills") == 0,
          "a %d MB pipelined burst to subscribers that keep up arrives whole: dropped %d, slow kills %d, received %s"
          % (NB * PL >> 20, d2("queue_dropped"), d2("slow_kills"), [r[0] for r in results]))
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
