#!/bin/sh
# yieldquiettest.sh - a master that yields says nothing until it is admitted: its heartbeat
# watchdog must not re-send its old MASTER_ALIVE while it is joining.
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
INJ=${3:-./clinject}
SEC=yq-client-secret
D=$(mktemp -d /var/tmp/pcyq.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "yieldquiettest: $CLI is missing (make perfcli)"; exit 1; }
[ -x "$INJ" ] || { echo "yieldquiettest: $INJ is missing (make clinject)"; exit 1; }

# no [cluster] collections: a forged frame's config digest (0) is not held against it
# (as handovertest's second fleet).  node <n>: 127.0.77.<n>:2481<n>; the founder gets the highest address (9)
conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = yq-cluster-secret
[listen]
tcp = 127.0.77.$1:2481$1
[cluster]
multicast = 239.255.83.9:24810
advertise = 127.0.77.$1
pull_timeout_ms = 400
mode = store
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"
}
wait_ready() {
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$1" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "no daemon: $(tail -2 "$1" | tr '\n' ' ')"; return 1
}

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
import json, subprocess, sys, time
cli, sec, inj = sys.argv[1], sys.argv[2], sys.argv[3]
def node(n): return ("127.0.77.%d" % n, "2481%d" % n)
M, A = node(9), node(1)
def call(n, req):
    try:
        o = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"], input=(json.dumps(req) + "\n").encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout.decode()
        return json.loads(o.splitlines()[0])
    except Exception as e:
        return {"_err": type(e).__name__}
def cl(n): r = call(n, {"method": "stats"}); return r.get("result", r).get("cluster") or {}
def role(n): return cl(n).get("role")
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.2)
    return -1

until(lambda: role(M) == "master" and role(A) == "member", 40)
until(lambda: cl(M).get("hb_sent", 0) >= 2, 10)
c0 = cl(M)
out("ROLES0", "%s/%s" % (c0.get("role"), role(A)))
out("SENT0", c0.get("hb_sent", -1))
term = max((c0.get("map") or {}).get("term", 0), c0.get("term", 0)) + 1
subprocess.run([inj, "-s", "yq-cluster-secret", "-c", c0.get("cluster_id", ""), "-f", "127.0.77.8:24818",
                "-t", "127.0.77.9:24810", "malive", "77", "3", str(term)], stdout=subprocess.DEVNULL, timeout=30)
out("YIELDED", until(lambda: role(M) == "joining", 5) >= 0)
c1 = cl(M)
out("DEMOTIONS", c1.get("demotions", 0) - c0.get("demotions", 0))
w0, s0, joining, t0 = c1.get("hb_watchdog", -1), c1.get("hb_sent", -1), True, time.time()
while time.time() - t0 < 6:
    time.sleep(0.5)
    c = cl(M)
    joining = joining and c.get("role") == "joining"
    w1, s1 = c.get("hb_watchdog", -1), c.get("hb_sent", -1)
out("JOINING_6S", joining)
out("WATCHDOG", "%s->%s" % (w0, w1))
out("SENT", "%s->%s" % (s0, s1))
one = until(lambda: sorted([role(M), role(A)]) == ["master", "member"] and cl(M).get("master") == cl(A).get("master"), 30)
out("ONE_MASTER", int(one) if one >= 0 else -1)
EOF

for n in 9 1; do conf $n; done
"$BIN" -f "$D/n9.conf" > "$D/n9.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n9.log" || { bad "the founder did not start"; echo "yieldquiettest: $pass passed, $fail failed"; exit 1; }
sleep 1
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n1.log" || { bad "the member did not start"; echo "yieldquiettest: $pass passed, $fail failed"; exit 1; }
python3 "$DRV" "$CLI" "$SEC" "$INJ" > "$D/out" 2>&1
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

[ "$(val ROLES0)" = "master/member" ] && [ "$(num "$(val SENT0)")" -ge 2 ] \
	&& ok "the pair formed: the founder is master, $(val SENT0) ALIVEs sent (frames for the watchdog)" \
	|| bad "the pair: $(val ROLES0), the master's ALIVEs $(val SENT0); $(tail -1 "$D/out")"
[ "$(val YIELDED)" = True ] && [ "$(val DEMOTIONS)" = 1 ] \
	&& ok "a beacon a term above makes the master step down and re-join (one demotion)" \
	|| bad "the forged beacon moved nothing (yielded $(val YIELDED), demotions +$(val DEMOTIONS)): $(grep -m1 'stepping down' "$D/n9.log" | sed 's/.*cluster: //' | cut -c1-90)"
W=$(val WATCHDOG) SN=$(val SENT)
[ -n "$SN" ] && [ "${SN%%->*}" = "${SN##*->}" ] && [ "${SN%%->*}" != -1 ] \
	|| bad "the window tests nothing: the joining node sent ALIVEs of its own (hb_sent $SN), so no beat was overdue"
[ "$(val JOINING_6S)" = True ] && [ -n "$W" ] && [ "${W%%->*}" = "${W##*->}" ] && [ "${W%%->*}" != -1 ] \
	&& ok "joining for 6 s, its watchdog re-sent nothing (hb_watchdog $W, hb_sent $SN) - no old master claim on the wire" \
	|| bad "while it was joining (throughout: $(val JOINING_6S)), its watchdog re-sent its last frames: hb_watchdog $W, hb_sent $SN"
O=$(num "$(val ONE_MASTER)")
[ "$O" -ge 0 ] && ok "then one master again, named by both, $O s later" \
	|| bad "no single master after 30 s: $(grep -E 'founded|claimed mastership|joined as|yielding' "$D/n9.log" "$D/n1.log" | sed 's/.*cluster: //' | tail -3 | tr '\n' '|' | cut -c1-200)"

echo "yieldquiettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
