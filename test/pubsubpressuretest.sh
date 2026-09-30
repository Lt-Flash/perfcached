#!/bin/sh
# pubsubpressuretest.sh - a publisher that outruns a worker's queue is
# paused, not silently dropped.  A publish for subscribers on another
# worker is queued there; past the queue's cap it was dropped and counted,
# while PUBLISH had already answered every receiver - on the 223 rig 32
# closed-loop publishers lost 66% of 1 KB messages that way.  A door's
# publish that leaves a queue at half its cap now pauses the publishing
# connection - its input waits and the kernel's flow control holds the
# client - until the queue is below a quarter.
#   - four workers, `pubsub_queue_mb = 1`, rpsbench flat out: 32
#     publishers of 1 KB into one subscriber, 16 into four
#   - nothing lost, no gaps or reorders, no subscriber closed, every
#     PUBLISH's receiver count honoured, nothing dropped at the queue,
#     and the pause counted
#   - a paused publisher does not stall its worker: PINGs on other
#     connections keep answering through the overload
# Fail-first: the build without the pause lost 1,592,493 and 2,513,316
# messages in the two cells, every one of them answered as delivered.
# Usage: test/pubsubpressuretest.sh [./perfcached] [./rpsbench]
set -u
BIN=${1:-./perfcached}
RPS=${2:-./rpsbench}
D=$(mktemp -d /var/tmp/pcpressure.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 4
pubsub_queue_mb = 1
[memory]
arena_mb = 32
[secrets]
client = pressure-client-secret
[listen]
tcp = 127.0.0.1:17591
resp = 127.0.0.1:17592
http = 127.0.0.1:17593
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(tail -1 "$D/n.log")"; echo "pubsubpressuretest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" PID="$P1" RPS="$RPS" timeout 120 python3 - <<'PY' 2> "$D/py.err"
import json, os, re, socket, subprocess, threading, time, urllib.request
res = open(os.environ["RESFILE"], "a")
pid, rps = os.environ["PID"], os.environ["RPS"]
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def stats():
    return json.load(urllib.request.urlopen("http://127.0.0.1:17593/stats", timeout=5))["pubsub"]

def cell(name, args):
    pings = [socket.create_connection(("127.0.0.1", 17592), timeout=10) for _ in range(4)]
    worst, stop = [0.0], [False]
    def pinger():
        while not stop[0]:
            for p in pings:
                t0 = time.time()
                p.sendall(b"*1\r\n$4\r\nPING\r\n")
                got = b""
                while not got.endswith(b"\r\n"):
                    got += p.recv(64)
                worst[0] = max(worst[0], time.time() - t0)
            time.sleep(0.02)
    st0 = stats()
    pt = threading.Thread(target=pinger)
    pt.start()
    out = subprocess.run([rps, "-p", "17592", "-x", pid, "-d", "3"] + args,
                         capture_output=True, text=True, timeout=90).stdout
    stop[0] = True
    pt.join(10)
    time.sleep(0.3)
    st = stats()
    d = lambda k: st.get(k, 0) - st0.get(k, 0)
    line = next((l for l in out.splitlines() if l.startswith("RESULT")), "")
    r = dict(re.findall(r"(\w+)=(\S+)", line))
    check(bool(r), "%s: rpsbench reported (%s)" % (name, line[:80] or out[-120:]))
    if not r:
        return
    check(float(r["pub_s"]) > 1000, "%s: publishers kept publishing (%s/s, %s deliveries/s)" % (name, r["pub_s"], r["deliv_s"]))
    check(r["lost"] == "0" and r["gaps"] == "0" and r["ooo"] == "0" and r["eof"] == "0",
          "%s: nothing lost, no gaps, reorders or closed subscribers (lost %s gaps %s ooo %s eof %s)" % (name, r["lost"], r["gaps"], r["ooo"], r["eof"]))
    check(r["recv_ok"] == "1" and r["got"] == r["expected"],
          "%s: every receiver PUBLISH counted got the message (counted %s, received %s)" % (name, r["expected"] if r["recv_ok"] == "1" else "not the subscribers", r["got"]))
    check(d("queue_dropped") == 0, "%s: nothing dropped at the queue (%d)" % (name, d("queue_dropped")))
    check(d("publish_paused") > 0, "%s: publishers were paused, and it is counted (%d)" % (name, d("publish_paused")))
    check(worst[0] < 1.0, "%s: other connections keep answering (worst PING %.3f s)" % (name, worst[0]))

cell("32 publishers, 1 KB, 1 subscriber", "-S 1 -s 1 -P 32 -t 2 -w 16 -v 1024".split())
cell("16 publishers, 1 KB, 4 subscribers", "-S 4 -s 2 -P 16 -t 2 -w 16 -v 1024".split())
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
echo "pubsubpressuretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
