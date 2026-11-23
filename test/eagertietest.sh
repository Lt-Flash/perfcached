#!/bin/sh
# eagertietest.sh - two writers, one version: an equal version with other bytes is a tie that the greater
# bytes win on every node, and the sweep's digest (CRC32C of each value) repairs a replica holding the loser.
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
SHIM=${3:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
SEC=et-client-secret EN=et-enable-secret
D=$(mktemp -d /var/tmp/pcet.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "eagertietest: $CLI is missing (make perfcli)"; exit 1; }
[ -f "$SHIM" ] || { echo "eagertietest: no netcut shim at $SHIM"; exit 1; }

# node <n>: 127.0.94.<n>:2483<n>; node 9 starts 1 s first and founds - the lowest identity, as a minted
# identity sorts by its start
for n in 9 1 2; do
	cat > "$D/n$n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = et-cluster-secret
enable = $EN
[listen]
tcp = 127.0.94.$n:2483$n
[cluster]
multicast = 239.255.85.9:24830
advertise = 127.0.94.$n
mode = eager
collections = c
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$n.conf"
done
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
cli, sec, en, d = sys.argv[1:5]
def node(n): return ("127.0.94.%d" % n, "2483%d" % n)
M, A, B = node(9), node(1), node(2)
# perfcli prints the result itself, not {"result": ...}; an error goes to stderr
def call(n, req, priv=False):
    try:
        p = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"] + (["-E", en] if priv else []),
                           input=(json.dumps(req) + "\n").encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=15)
        o = p.stdout.decode().splitlines()
        return json.loads(o[0]) if o else {"_err": p.stderr.decode().strip()[:120]}
    except Exception as e:
        return {"_err": type(e).__name__}
def cl(n): return call(n, {"method": "stats"}).get("cluster") or {}
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.5)
    return -1
def cut(on):
    open(d + "/cut1", "w").write("cut 127.0.94.2 127.0.94.9 rst\n" if on else "")
    time.sleep(0.3)                    # the shim re-reads at most every 50 ms
def vals(n, pfx):
    r = call(n, {"method": "mget", "params": {"col": "c", "keys": ["%s:%d" % (pfx, i) for i in range(50)]}})
    return [x.get("value") if x.get("found") else None for x in r.get("values", [])] or [r]
def want(n, pfx, tag): return vals(n, pfx) == ["%s-%d" % (tag, i) for i in range(50)]
def held(n, pfx):
    v = [str(x).split("-")[0] for x in vals(n, pfx)]
    return "%s x%d" % (v[0][:80], v.count(v[0]))
def state(n): return call(n, {"method": "stats"}).get("state")
def diffs(): return sum((cl(x).get("sweep_gate") or {}).get("slots_diff", 0) for x in (M, A, B))
def fork(pfx):
    ver = max(cl(x).get("lamport", 0) for x in (M, A, B)) + 1000
    cut(True)
    w = []
    # node 1 first: a sweep batch retried after the heal can carry only a, which loses
    for n, tag in ((A, "a"), (B, "z")):
        r = call(n, {"method": "restore", "params": {"col": "c", "policy": "newer", "records":
                 [{"k": "%s:%d" % (pfx, i), "v": "%s-%d" % (tag, i), "ver": ver} for i in range(50)]}}, True)
        w.append(r.get("stored", r))
    time.sleep(1)
    cut(False)
    return w

# READY too: a recovering node refuses the restore, and libperfd re-sends it to another member
out("FORMED", until(lambda: cl(M).get("role") == "master" and cl(A).get("peers_up") == 2 and cl(B).get("peers_up") == 2
                    and [state(x) for x in (M, A, B)] == ["ready"] * 3, 60) >= 0)
out("STATES", "/".join(str(state(x)) for x in (A, B, M)))
out("EPOCH0", cl(M).get("wire_epoch"))
o0, d0 = cl(A).get("recv_older", 0), diffs()
out("STORED", fork("t"))
out("HEAL", held(A, "t"))                      # a, unless the gate was quicker than this read
out("FORK", want(B, "t", "z") and want(M, "t", "z"))
fixed = until(lambda: want(A, "t", "z"), 60)
out("REPAIRED", int(fixed) if fixed >= 0 else -1)
out("ALL", want(M, "t", "z") and want(B, "t", "z"))
out("DIFFS", diffs() - d0)
out("HELD", "%s / %s / %s, node 1 refused %d as older" % (
    held(A, "t"), held(B, "t"), held(M, "t"), cl(A).get("recv_older", 0) - o0))
EOF

for n in 9 1 2; do
	if [ $n = 9 ]; then "$BIN" -f "$D/n9.conf" > "$D/n9.log" 2>&1 &
	elif [ $n = 1 ]; then LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut1" PC_NETCUT_LOG="$D/cutlog1" \
		"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
	else : > "$D/n$n.log"; "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	fi
	PIDS="$PIDS $!"
	wait_ready "$D/n$n.log" || { bad "node $n did not start"; echo "eagertietest: $pass passed, $fail failed"; exit 1; }
	[ $n = 9 ] && sleep 1
done
python3 "$DRV" "$CLI" "$SEC" "$EN" "$D" > "$D/out" 2>&1
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

[ "$(val FORMED)" = True ] && [ "$(val EPOCH0)" = 4 ] && ok "three eager members, the fleet on wire epoch 4" \
	|| bad "the fleet: formed $(val FORMED) (node 1 / 2 / 9 $(val STATES)), epoch $(val EPOCH0); $(tail -1 "$D/out")"
[ "$(val STORED)" = "[50, 50]" ] && [ "$(val FORK)" = True ] && grep -qE "in [1-9]|rst [1-9]" "$D/cutlog1" \
	&& ok "one version, two values: nodes 2 and 9 hold z, node 1 held $(val HEAL) at the heal (the cut delivered: $(cat "$D/cutlog1"))" \
	|| bad "no fork to test: stored $(val STORED), nodes 2 and 9 on z $(val FORK), node 1 $(val HEAL), cut $(cat "$D/cutlog1" 2>&1)"
R=$(num "$(val REPAIRED)")
[ "$R" -ge 0 ] && [ "$(val ALL)" = True ] \
	&& ok "(a)+(b) node 1 took z over its own a in $R s, and nodes 2 and 9 kept z ($(val HELD))" \
	|| bad "(a)+(b) 60 s after the fork: $(val HELD) (node 1 / 2 / 9)"
# the sweep's repeat can carry z before the gate compares the slot: what moved it is reported only
echo "  info the gate's slots_diff rose by $(val DIFFS) meanwhile"

echo "eagertietest: $pass passed, $fail failed"
[ $fail -eq 0 ]
