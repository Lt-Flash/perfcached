#!/bin/sh
# pubsubpressuretest.sh - a publisher that outruns another worker's queue is paused, not dropped, and
# does not stall its worker.  A slow-subscriber kill voids a try (PRESSURE_STARVE_SUB=1 is its control).
set -u
BIN=${1:-./perfcached}
RPS=${2:-./rpsbench}
D=$(mktemp -d /var/tmp/pcpressure.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
RESFILE="$RES" PID="$P1" RPS="$RPS" timeout 240 python3 - <<'PY' 2> "$D/py.err"
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
    spin = None
    pr = subprocess.Popen([rps, "-p", "17592", "-x", pid, "-d", "3"] + args,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if starve:
        # the positive control: the subscriber thread (rpsbench starts
        # its subscribers first) on a core a spinner holds, SCHED_IDLE
        spin = subprocess.Popen(["python3", "-c",
            "import os,time\nos.sched_setaffinity(0,{0})\nt=time.time()+6\nwhile time.time()<t: pass"])
        time.sleep(0.3)
        tids = sorted(int(t) for t in os.listdir("/proc/%d/task" % pr.pid))
        if len(tids) > 1:
            os.sched_setaffinity(tids[1], {0})
            os.sched_setscheduler(tids[1], os.SCHED_IDLE, os.sched_param(0))
    out = pr.communicate(timeout=90)[0]
    if spin:
        spin.kill(); spin.wait()
    stop[0] = True
    pt.join(10)
    time.sleep(0.3)
    st = stats()
    d = lambda k: st.get(k, 0) - st0.get(k, 0)
    line = next((l for l in out.splitlines() if l.startswith("RESULT")), "")
    r = dict(re.findall(r"(\w+)=(\S+)", line))
    check(bool(r), "%s: rpsbench reported (%s)" % (name, line[:80] or out[-120:]))
    if not r:
        return True
    # queue drops are this suite's subject: judged on every run
    check(d("queue_dropped") == 0, "%s: nothing dropped at the queue (%d)" % (name, d("queue_dropped")))
    if d("slow_kills") > 0 and not last:
        res.write("I %s: NOT JUDGED - the daemon closed the subscriber as slow (slow_kills %d, received %s): the machine starved it; running again\n"
                  % (name, d("slow_kills"), r.get("got")))
        res.flush()
        return False
    check(d("slow_kills") == 0, "%s: the subscriber kept up - no slow-subscriber kill (slow_kills %d)" % (name, d("slow_kills")))
    check(float(r["pub_s"]) > 1000, "%s: publishers kept publishing (%s/s, %s deliveries/s)" % (name, r["pub_s"], r["deliv_s"]))
    check(r["lost"] == "0" and r["gaps"] == "0" and r["ooo"] == "0" and r["eof"] == "0",
          "%s: nothing lost, no gaps, reorders or closed subscribers (lost %s gaps %s ooo %s eof %s)" % (name, r["lost"], r["gaps"], r["ooo"], r["eof"]))
    check(r["recv_ok"] == "1" and r["got"] == r["expected"],
          "%s: every receiver PUBLISH counted got the message (counted %s, received %s)" % (name, r["expected"] if r["recv_ok"] == "1" else "not the subscribers", r["got"]))
    check(d("publish_paused") > 0, "%s: publishers were paused, and it is counted (%d)" % (name, d("publish_paused")))
    check(worst[0] < 1.0, "%s: other connections keep answering (worst PING %.3f s)" % (name, worst[0]))
    return True

starve = os.environ.get("PRESSURE_STARVE_SUB") == "1"
for name, args in (("32 publishers, 1 KB, 1 subscriber", "-S 1 -s 1 -P 32 -t 2 -w 16 -v 1024"),
                   ("16 publishers, 1 KB, 4 subscribers", "-S 4 -s 2 -P 16 -t 2 -w 16 -v 1024")):
    for attempt in range(3):
        last = attempt == 2
        if cell(name, args.split()):
            break
PY
drv=$?; [ $drv -eq 0 ] || echo "F the driver stopped: exit $drv" >> "$RES"   # S488
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; I\ *) echo "  ..   ${l#I }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
echo "pubsubpressuretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
