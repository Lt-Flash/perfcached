#!/bin/sh
# idduptest.sh - two live holders of one node id: the member with the higher identity re-joins, and
# the master never hands a re-joiner an id another live node holds.
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
INJ=${3:-./clinject}
SEC=id-client-secret
D=$(mktemp -d /var/tmp/pcid.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "idduptest: $CLI is missing (make perfcli)"; exit 1; }
[ -x "$INJ" ] || { echo "idduptest: $INJ is missing (make clinject)"; exit 1; }

# no [cluster] collections: a forged frame's config digest (0) is not held against it.
# node <n>: 127.0.93.<n>:2482<n>; the founder gets the highest address (9)
conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = id-cluster-secret
[listen]
tcp = 127.0.93.$1:2482$1
[cluster]
multicast = 239.255.84.9:24820
advertise = 127.0.93.$1
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
def node(n): return ("127.0.93.%d" % n, "2482%d" % n)
M, A, B = node(9), node(1), node(2)
def call(n, req):
    try:
        o = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"], input=(json.dumps(req) + "\n").encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout.decode()
        return json.loads(o.splitlines()[0])
    except Exception as e:
        return {"_err": type(e).__name__}
def cl(n): r = call(n, {"method": "stats"}); return r.get("result", r).get("cluster") or {}
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.2)
    return -1
def formed():
    c = [cl(n) for n in (M, A, B)]
    return [x.get("role") for x in c] == ["master", "member", "member"] and \
        len({x.get("node") for x in c}) == 3 and all((x.get("map") or {}).get("usable") for x in c)
def claim(nodeid, ident, secs):
    cid = cl(M).get("cluster_id", "")
    return [subprocess.Popen([inj, "-s", "id-cluster-secret", "-c", cid, "-f", "127.0.93.%d:2482%d" % (s, s),
                              "-t", "%s:24820" % t[0], "-n", str(secs * 4), "-i", "250", "alive", str(nodeid), ident],
                             stdout=subprocess.DEVNULL) for s, t in ((7, M), (6, A), (5, B))]

out("FORMED", until(formed, 40) >= 0)
idA, idB = cl(A).get("node"), cl(B).get("node")
out("IDS0", "%s/%s/%s" % (cl(M).get("node"), idA, idB))
ps = claim(idA, "00" * 16, 10)
moved = until(lambda: cl(A).get("node") not in (None, idA) and cl(A).get("role") == "member", 9)
out("A_NEW", "%d %s->%s" % (int(moved * 1000) if moved >= 0 else -1, idA, cl(A).get("node")))
for p in ps: p.wait()
ps = claim(idB, "ff" * 16, 6)
for p in ps: p.wait()
out("B_KEPT", "%s->%s %s" % (idB, cl(B).get("node"), cl(B).get("role")))
done = until(formed, 30)
out("SETTLED", int(done) if done >= 0 else -1)
out("IDS1", "%s/%s/%s" % (cl(M).get("node"), cl(A).get("node"), cl(B).get("node")))
EOF

for n in 9 1 2; do conf $n; done
: > "$D/n9.log"
"$BIN" -f "$D/n9.conf" > "$D/n9.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n9.log" || { bad "the founder did not start"; echo "idduptest: $pass passed, $fail failed"; exit 1; }
sleep 1
for n in 1 2; do "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 & PIDS="$PIDS $!"; done
wait_ready "$D/n1.log" && wait_ready "$D/n2.log" || { bad "the members did not start"; echo "idduptest: $pass passed, $fail failed"; exit 1; }
python3 "$DRV" "$CLI" "$SEC" "$INJ" > "$D/out" 2>&1
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

[ "$(val FORMED)" = True ] && ok "three nodes, one master, distinct ids, the map usable everywhere ($(val IDS0))" \
	|| bad "the fleet did not form: $(val IDS0); $(tail -1 "$D/out")"
A=$(val A_NEW); MS=$(num "${A%% *}")
[ "$MS" -ge 0 ] && grep -q "is claimed by 127.0.93.6.* re-joining" "$D/n1.log" \
	&& ok "its id claimed under a lower identity: node 1 re-joined under another ($A ms)" \
	|| bad "its id claimed under a lower identity, node 1: $A; $(grep -hE 'claimed by|re-joining|joined as' "$D/n1.log" | tail -2 | sed 's/.*cluster: //' | tr '\n' '|' | cut -c1-200)"
B=$(val B_KEPT)
[ "${B%%->*}" = "$(echo "$B" | sed 's/.*->\([0-9]*\) .*/\1/')" ] && [ "${B##* }" = member ] && ! grep -q "re-joining for an id" "$D/n2.log" \
	&& ok "its id claimed under a higher identity: node 2 kept it ($B) - one holder moves, not both" \
	|| bad "under a higher identity node 2 moved too: $B"
S=$(num "$(val SETTLED)")
[ "$S" -ge 0 ] && ok "the sender gone: three distinct ids, one master, the map usable everywhere ($(val IDS1), $S s)" \
	|| bad "not settled 30 s after the sender stopped: $(val IDS1)"

echo "idduptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
