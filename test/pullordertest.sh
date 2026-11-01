#!/bin/sh
# pullordertest.sh - a replica read in its start window answers each read
# with that read's reply, while member pushes arrive between them.
#
# applythreadstest saw a read on the replica go "backwards" in 3 of about 8
# CI runs.  It was never data: S462 pushes a member's state as a NOTIFY
# frame to every native connection, the push arrived between a request and
# its reply, and the suite's client took it for the reply - every later
# read then got the reply of the read before (request 2802 answered with
# 2801's value, 2801 with the id-less push; GitLab 112538, 12 of 12 rounds).
# Each round is a fresh fleet of two (io_uring and epoll in turn,
# apply_threads 1 and 4) written through A while a reader on B reads at
# full speed, every request with its own id: a reply whose id is not its
# request's, or a value of another key or older than one already read,
# fails the round.
# Usage: test/pullordertest.sh [./perfcached] [rounds]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
ROUNDS=${2:-12}
SEC=po-client-secret
D=$(mktemp -d /var/tmp/pcpo.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

# node <n>: 127.0.158.<n>:2488<n>, the cluster port 24880
conf() { # conf <n> <apply_threads> <io>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
io = $3
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = po-cluster-secret
[listen]
tcp = 127.0.158.$1:2488$1
plaintext = loopback
[cluster]
multicast = 239.255.81.15:24880
advertise = 127.0.158.$1
pull_timeout_ms = 400
mode = eager
collections = c
apply_threads = $2
[collection c]
buckets_log2 = 12
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # <node>
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	eval "P$1=$!"; PIDS="$PIDS $!"
	i=0
	while [ $i -lt 300 ]; do
		[ "$(grep -c 'perfcached ready' "$D/n$1.log" 2>/dev/null)" -ge "$2" ] && return 0
		sleep 0.1; i=$((i + 1))
	done
	return 1
}

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# drv.py <round>: writes new keys through A while B, just started, reads them
import json, pcnative, socket, sys, threading, time
rnd = int(sys.argv[1])
A = ("127.0.158.1", 24881); B = ("127.0.158.2", 24882)
HOT, VERS, TOG = 40, 120, 40
class Conn:                          # each request its own id, checked on the reply
    def __init__(s, n):
        s.f = pcnative.wrap(socket.create_connection(n, timeout=30)); s.n = 0
    def call(s, req):
        s.n += 1
        req = dict(req); req["jsonrpc"] = "2.0"; req["id"] = s.n
        s.f.write((json.dumps(req) + "\n").encode()); s.f.flush()
        while True:                   # a push (S462's member state) is no reply
            r = json.loads(s.f.readline())
            if "id" in r: break
        res = r.get("result", r)
        if isinstance(res, dict) and r.get("id") != s.n:
            res = dict(res); res["_id"] = "reply id %s for request %s" % (r.get("id"), s.n)
        return res
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def formed():
    try:
        sa, sb = Conn(A).call({"method": "stats"}), Conn(B).call({"method": "stats"})
        return sa.get("state") == sb.get("state") == "ready" and \
            (sa.get("cluster") or {}).get("peers_up") == (sb.get("cluster") or {}).get("peers_up") == 1
    except Exception:
        return False
for _ in range(300):                       # one fleet, not two founders yet to merge
    if formed(): break
    time.sleep(0.1)
out("FORMED", formed())
hot = lambda k: "r%dhot%d" % (rnd, k)
stop = [0]; reads = [0]; back = []; hist = {}
def reader():
    rb = Conn(B); last = {}; t0 = time.time()
    while not stop[0] and not back:
        for k in range(HOT):
            g = rb.call({"method": "get", "params": {"col": "c", "key": hot(k)}})
            v = g.get("value")
            h = hist.setdefault(k, [])
            h.append("%s/%s%s" % (v.split(":", 1)[1] if v else "miss", (g.get("source") or "-")[:1],
                " " + g["_id"] if "_id" in g else ""))
            del h[:-6]
            if "_id" in g:
                back.append("%s: %s (value %r)" % (hot(k), g["_id"], v)); break
            if v is None: continue
            reads[0] += 1
            n = int(v.split(":")[1])
            if not v.startswith(hot(k) + ":") or n < last.get(k, 0):
                after = rb.call({"method": "exists", "params": {"col": "c", "key": hot(k)}})
                back.append("%s %s after %d at %.2fs; its last reads (value/source) %s; the key before it %s; B's ver just after %s" % (
                    hot(k), v, last.get(k, 0), time.time() - t0, " ".join(h),
                    " ".join(hist.get(k - 1, [])), after.get("ver") if after.get("exists") else "-"))
                break
            last[k] = n
t = threading.Thread(target=reader); t.start()
a = Conn(A)
for v in range(1, VERS + 1):
    for k in range(HOT):
        a.call({"method": "set", "params": {"col": "c", "key": hot(k), "value": "%s:%d" % (hot(k), v)}})
    if v % 10 == 0:
        for k in range(TOG):
            tk = "r%dtog%d" % (rnd, k)
            a.call({"method": "set", "params": {"col": "c", "key": tk, "value": "t:%d" % v}})
            if v % 20 == 0:
                a.call({"method": "del", "params": {"col": "c", "key": tk}})
                a.call({"method": "set", "params": {"col": "c", "key": tk, "value": "t:%d" % v}})
time.sleep(0.5)
stop[0] = 1; t.join()
ae = Conn(A).call({"method": "exists", "params": {"col": "c", "key": back[0].split()[0]}}) if back else {}
cb = Conn(B).call({"method": "stats"}).get("cluster") or {}
out("READS", reads[0])
out("BACK", back[0] if back else "none")
out("A_VER", ae.get("ver"))
mb = Conn(B).call({"method": "stats"}).get("memory") or {}
out("B_STATS", "pulls %s hits %s misses %s timeouts %s, recv_older %s, recv_tombstoned %s, final %s; arena: refused %s, older dropped %s, used %s of %s" % (
    cb.get("pull_sent"), cb.get("pull_hits"), cb.get("pull_misses"), cb.get("pull_timeouts"),
    cb.get("recv_older"), cb.get("recv_tombstoned"), cb.get("eager_miss_final"), cb.get("recv_nomem"),
    cb.get("recv_stale_dropped"), mb.get("arena_used"), mb.get("arena_total")))
out("NOMEM", cb.get("recv_nomem"))
EOF
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }

r=1 seen=0 tot=0 nu=0 ne=0 bu=0 be=0 first=
while [ $r -le "$ROUNDS" ]; do
	for p in ${P1:-} ${P2:-}; do kill -9 "$p" 2>/dev/null; done; sleep 0.3
	at=$(( r % 2 ? 1 : 4 ))
	io=$( [ $(( (r - 1) / 2 % 2 )) = 0 ] && echo io_uring || echo epoll )
	conf 1 $at $io; conf 2 $at $io
	start 1 "$r" && start 2 "$r" || { bad "round $r: the fleet did not start: $(tail -1 "$D/n1.log") $(tail -1 "$D/n2.log")"; break; }
	python3 "$DRV" "$r" > "$D/out" 2>&1
	tot=$((tot + $(val READS | tr -dc 0-9)))
	[ $io = io_uring ] && nu=$((nu + 1)) || ne=$((ne + 1))
	if [ "$(val BACK)" != none ]; then
		[ $io = io_uring ] && bu=$((bu + 1)) || be=$((be + 1))
		[ -n "$first" ] || first="round $r (apply_threads $at, $io): $(val BACK); A's ver $(val A_VER); B $(val B_STATS)"
	fi
	[ -n "$(val READS)" ] || { bad "round $r: the driver failed: $(tail -2 "$D/out")"; seen=1; break; }
	# a zero proves nothing: 12 rounds read 0 times when two founders had not merged
	[ "$(val FORMED)" = True ] && [ "$(val READS)" -gt 100 ] 2>/dev/null \
		|| { bad "round $r: the fleet formed $(val FORMED), B read $(val READS) values - nothing measured"; seen=1; break; }
	# the premise (HG_MALLOC's lesson): a replica out of arena measures memory, not order
	[ "$(val NOMEM)" = 0 ] || { bad "round $r: B refused copies for want of arena ($(val B_STATS)) - this run measured memory"; seen=1; break; }
	r=$((r + 1))
done
[ $seen = 0 ] && [ -z "$first" ] && ok "$ROUNDS rounds, each a fresh fleet read at once on its replica ($tot reads): never a value older than one already served"
[ -n "$first" ] && bad "B answered a read with another request's reply in $bu of $nu io_uring rounds and $be of $ne epoll rounds; the first: $first"

echo "pullordertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
