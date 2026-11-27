#!/bin/sh
# upechomastertest.sh - a client reads the key it has just written through: a fetch inside the echo
# window goes to the master, not a lagging replica (fakes: R1 50 ms, R2 700 ms behind M).
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcechom.XXXXXX)
PM=17931 P1=17932 P2=17933 TCP=17934 RESP=17935 HTTP=17936
PID= FM= F1= F2=
trap 'kill $PID 2>/dev/null; for p in $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $P2 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upechomastertest: port $p already bound" >&2; exit 1; }
done
ctl() { echo "$2" > "$D/up$1/ctl.json"; }
fakes() {
	unfake
	for n in m 1 2; do
		rm -rf "$D/up$n"; mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"
	done
	ctl m '{"role": "master", "accept_writes": true, "push_on_write": true, "push_delay_ms": 5, "replicate_delay_ms": {"'"$D"'/up1": 50, "'"$D"'/up2": 700}, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
	ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
	ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
	python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
	python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
	python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
	i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
}
unfake() { for p in $FM $F1 $F2; do kill -9 $p 2>/dev/null; wait $p 2>/dev/null; done; FM= F1= F2=; }
conf() { # conf <name> [extra collection lines...]
	n=$1; shift
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = echom-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_master_fallback = yes
upstream_mode = balance
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 500
upstream_negative_ms = 2000
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
start() {
	rm -rf "$D/state"; mkdir -p "$D/state"
	: > "$D/$1.log"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
st() { # st <python expr over u (collection 0's upstream block)>
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
# the writer must have found M (ROLE) before anything is written
ready_writer() {
	i=0; while [ $i -lt 100 ] && [ "$(st 'sum(1 for x in u["upstreams"] if x.get("role")=="master")')" != 1 ]; do sleep 0.1; i=$((i+1)); done
	sleep 1
}
drive() { # drive <A|B|C|D|E|F|G|N> - prints one result line
	python3 - "$1" $RESP $PM "$D" "$T" $TCP <<'PY'
import json, os, socket, sys, time
what, port, pm, d = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
class R:
    def __init__(s, p):
        s.k = socket.create_connection(("127.0.0.1", p), 5); s.k.settimeout(5); s.f = s.k.makefile("rb")
    def cmd(s, *a):
        s.k.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (str(y).encode() for y in a)))
        return s.rd()
    def rd(s):
        l = s.f.readline(); t, v = l[:1], l[1:-2]
        if t == b"+": return v.decode()
        if t == b"-": return "ERR " + v.decode()
        if t == b":": return int(v)
        if t == b"$":
            n = int(v)
            return None if n < 0 else s.f.read(n + 2)[:-2].decode()
        if t == b"_": return None
        raise SystemExit("bad reply %r" % l)
c = R(port)
def window(k, v):
    """SET, then a read every 10 ms for 800 ms: every answer must be v"""
    r = c.cmd("SET", k, v)
    if r != "OK":                      # not a stale read: the write itself (rc3: 1 of 40 on 224)
        return (0, "SET answered %r" % r)
    t0 = time.time(); seen = []
    while time.time() - t0 < 0.8:
        g = c.cmd("GET", k)
        if g != v:
            seen.append((round((time.time() - t0) * 1000), g))
        time.sleep(0.01)
    return seen[0] if seen else None
if what in ("A", "E"):
    bad = [(i, w) for i in range(40) for w in [window("a:%s:%d" % (what, i), "v%d" % i)] if w]
    print("%d/40 wrong%s" % (len(bad), (" - first: key %d read %r at +%s ms" % (bad[0][0], bad[0][1][1], bad[0][1][0])) if bad else ""))
elif what == "B":
    time.sleep(1.0)                    # both replicas hold A's values now
    bad = [(i, w) for i in range(40) for w in [window("a:A:%d" % i, "w%d" % i)] if w]
    print("%d/40 wrong%s" % (len(bad), (" - first: key %d read %r at +%s ms" % (bad[0][0], bad[0][1][1], bad[0][1][0])) if bad else ""))
elif what == "C":
    m = R(pm)
    c.cmd("SET", "race", "ours"); m.cmd("SET", "race", "theirs")   # another client, straight on M
    t0 = time.time(); g = None
    while time.time() - t0 < 1.0:
        g = c.cmd("GET", "race")
        if g == "theirs": break
        time.sleep(0.02)
    print("%s after %.0f ms" % (g, (time.time() - t0) * 1000))
elif what == "D":
    m = R(pm); m.cmd("SET", "cold", "c1")                        # another client; never written here
    time.sleep(2.6)                    # past the replicas' lag and any echo window
    print(c.cmd("GET", "cold"))
elif what == "F":
    import threading
    m = R(pm); m.cmd("SET", "lost:1", "x1")                      # another client; never written here
    time.sleep(1.0)                    # both replicas hold it, their pushes are done
    first = c.cmd("GET", "lost:1")     # fetched: a copy is held now
    res = {}
    def held():
        k = R(port); res["g"] = k.cmd("GET", "lost:1")
    t0 = time.time()
    th = threading.Thread(target=held); th.start()
    time.sleep(0.1)                    # inside the 300 ms hold
    m.cmd("SET", "lost:1", "x2")       # M pushes: the copy goes from every shadow
    th.join(10)
    print("%s %s %.0f" % (first, res.get("g", "noreply"), (time.time() - t0) * 1000))
elif what == "N":
    # F at the native door: a get held between the gate and its read, its copy taken
    import threading
    sys.path.insert(0, sys.argv[5])
    import pcnative
    m = R(pm); m.cmd("SET", "nlost:1", "x1")                     # another client; never written here
    time.sleep(1.0)
    def nget():
        f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[6])), 10))
        r = pcnative.call(f, "get", col="0", key="nlost:1").get("result") or {}
        return r.get("value") if r.get("found") else None
    first = nget()
    res = {}
    def held():
        res["g"] = nget()
    th = threading.Thread(target=held); th.start()
    time.sleep(0.1)                    # inside the 300 ms hold
    m.cmd("SET", "nlost:1", "x2")
    th.join(10)
    print("%s %s" % (first, res.get("g", "noreply")))
elif what == "H":
    import threading
    bad = []
    for i in range(5):
        k, v = "gap:%d" % i, "h%d" % i
        c.cmd("SET", k, v)
        res = []
        def held():
            res.append(R(port).cmd("GET", k))
        th = [threading.Thread(target=held) for _ in range(4)]
        [t.start() for t in th]; [t.join(10) for t in th]
        if any(g != v for g in res) or len(res) != 4:
            bad.append((k, res))
    print("%d/5 wrong%s" % (len(bad), (" - first: %s read %r" % bad[0]) if bad else ""))
elif what == "G":
    m = R(pm)
    for i in range(40):
        m.cmd("SET", "del:%d" % i, "old%d" % i)                 # another client: no window here
    time.sleep(1.0)                    # both replicas hold them, their pushes are done
    bad = []
    for i in range(40):
        k = "del:%d" % i
        n = c.cmd("DEL", k)
        t0 = time.time(); seen = None
        while time.time() - t0 < 0.8:
            g = c.cmd("GET", k)
            if g is not None and seen is None:
                seen = (round((time.time() - t0) * 1000), g)
            time.sleep(0.01)
        if n != 1 or seen:
            bad.append((i, n, seen))
    print("%d/40 wrong%s" % (len(bad), (" - first: key %d DEL %r, read %r" % bad[0]) if bad else ""))
PY
}
fetches() { st '",".join("%s=%s" % (x["addr"].rsplit(":",1)[1], x.get("fetches")) for x in u["upstreams"])'; }

fakes; conf on
start on || { bad "the node did not start"; tail -5 "$D/on.log"; exit 1; }
ready_writer
r=$(drive A); [ "$r" = "0/40 wrong" ] && ok "A. a new key, SET then GET at once: every GET the value just set ($r)" ||
	bad "A. read after write: $r; write_through $(st '{k: v for k, v in u["write_through"].items() if v}')"
r=$(drive B); [ "$r" = "0/40 wrong" ] && ok "B. an existing key overwritten: the new value every time ($r)" || bad "B. overwrite: $r"
em=$(st 'u["write_through"].get("echo_master_fetches")')
[ "${em:-0}" -ge 40 ] 2>/dev/null && ok "   echo_master_fetches $em: the echo window's fetches went to M" || bad "   echo_master_fetches '$em'"
r=$(drive C); ms=$(echo "$r" | sed -n 's/^theirs after \([0-9]*\) ms$/\1/p')
[ -n "$ms" ] && [ "$ms" -le 100 ] && ok "C. a change racing the write is honoured at once: '$r'" || bad "C. the racing change: '$r' (more than 100 ms - the older copy served meanwhile)"
sleep 2.5                              # past C's echo window: its slow replica's echo is routed to M
f0=$(fetches); r=$(drive D); f1=$(fetches)
m0=$(echo "$f0" | tr ',' '\n' | grep "^$PM=" | cut -d= -f2); m1=$(echo "$f1" | tr ',' '\n' | grep "^$PM=" | cut -d= -f2)
[ "$r" = c1 ] && [ "$m0" = "$m1" ] && ok "D. outside the window a cold read goes to a replica: c1, fetches $f0 -> $f1 (M unchanged)" || bad "D. cold read '$r', fetches $f0 -> $f1"
r=$(drive G); [ "$r" = "0/40 wrong" ] && ok "G. S451: a DELETE, then GET at once: nil every time, never the value just deleted ($r)" || bad "G. read after delete: $r"
stop
fakes; conf off "upstream_echo_ms = 0"
start off || { bad "the node (echo off) did not start"; tail -5 "$D/off.log"; exit 1; }
ready_writer
r=$(drive E); case "$r" in "0/40 wrong") bad "E. with the echo handling off the stale read did not appear ($r) - this rig cannot show the defect";; *) ok "E. upstream_echo_ms = 0 (the old drop) reads stale after its own write: $r - the suite can fail";; esac
r=$(drive G); case "$r" in "0/40 wrong") bad "G. with the echo handling off no read after a delete answered the deleted value ($r) - this rig cannot show S451";; *) ok "G. upstream_echo_ms = 0 reads the deleted value after its own delete: $r - the suite can fail";; esac
stop
# F: the read held between the gate and its handler, its copy taken
fakes; conf lostoff
PERFCACHED_TEST_GATE_DELAY_US=300000; PERFCACHED_TEST_LOST_OFF=1
export PERFCACHED_TEST_GATE_DELAY_US PERFCACHED_TEST_LOST_OFF
start lostoff || { bad "the node (gate delay, re-gate off) did not start"; tail -5 "$D/lostoff.log"; exit 1; }
unset PERFCACHED_TEST_GATE_DELAY_US PERFCACHED_TEST_LOST_OFF
ready_writer
r=$(drive F); set -- $r
[ "${1:-}" = x1 ] && [ "${2:-}" = None ] &&
	ok "F. re-gate off: the held read whose copy an invalidation took answers nil ($r) - the suite can fail" ||
	bad "F. re-gate off: '$r' - expected x1, then None (this rig cannot show the defect)"
stop
fakes; conf lost
PERFCACHED_TEST_GATE_DELAY_US=300000; export PERFCACHED_TEST_GATE_DELAY_US
start lost || { bad "the node (gate delay) did not start"; tail -5 "$D/lost.log"; exit 1; }
unset PERFCACHED_TEST_GATE_DELAY_US
ready_writer
r=$(drive F); set -- $r; nl=$(st 'u.get("read_lost")')
[ "${1:-}" = x1 ] && { [ "${2:-}" = x1 ] || [ "${2:-}" = x2 ]; } && [ "${nl:-0}" -ge 1 ] 2>/dev/null &&
	ok "F. the held read whose copy an invalidation took is gated again and fetches: '${2:-}' (read_lost $nl)" ||
	bad "F. the held read: '$r', read_lost '$nl' - expected x1, then x1 or x2, read_lost >= 1"
r=$(drive N); set -- $r; nl2=$(st 'u.get("read_lost")')
[ "${1:-}" = x1 ] && { [ "${2:-}" = x1 ] || [ "${2:-}" = x2 ]; } && [ "${nl2:-0}" -gt "${nl:-0}" ] 2>/dev/null &&
	ok "F. the native door too: a held get whose copy went is gated again: '${2:-}' (read_lost $nl -> $nl2)" ||
	bad "F. the native held get: '$r', read_lost $nl -> '$nl2' - expected x1, then x1 or x2, read_lost up"
stop
# H: the read held past its record's drop while the mark lingers
fakes; conf gap
PERFCACHED_TEST_GATE_DELAY_US=300000; PERFCACHED_TEST_INV_GAP_US=600000
export PERFCACHED_TEST_GATE_DELAY_US PERFCACHED_TEST_INV_GAP_US
start gap || { bad "the node (gate delay, invalidation gap) did not start"; tail -5 "$D/gap.log"; exit 1; }
unset PERFCACHED_TEST_GATE_DELAY_US PERFCACHED_TEST_INV_GAP_US
ready_writer
r=$(drive H); [ "$r" = "0/5 wrong" ] && ok "H. S481: a held read whose record an echo took, its mark left: the value every time ($r)" ||
	bad "H. a held read, its record gone, its mark left: $r; read_lost $(st 'u.get("read_lost")'), of them with the re-gates spent $(st 'u.get("read_lost_spent")'), fetches by port $(fetches)"
stop
echo "upechomastertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
