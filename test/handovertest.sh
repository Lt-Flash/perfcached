#!/bin/sh
# handovertest.sh - CH-9: the operator moves mastership before a restart.
#
# Three nodes; the founder (highest address) is master.  `master_prefer
# <node>` on the master names that member standby, and once the published
# map says so the master sends M_HANDOVER: the member promotes at the next
# term and the old master steps down on its beacon.
#  - refused without the enable secret, on a member, and for a node that
#    is not a live member;
#  - the named member is master within a few seconds, the old master and
#    the third node are members, and all three agree;
#  - at no sample was the fleet without a master (no election's silence);
#  - the new master's term is above the old one's; the logs name the
#    handover on both sides; a key reads back everywhere and a write
#    through the new master lands everywhere.
# Fail-first: without CH-9 the verb is unknown and nothing moves.
# Usage: test/handovertest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
SEC=ho-client-secret
EN=ho-enable-secret
D=$(mktemp -d /var/tmp/pcho.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "handovertest: $CLI is missing (make perfcli)"; exit 1; }

# node <n>: 127.0.75.<n>:2475<n>; the founder gets the highest address (9)
conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ho-cluster-secret
enable = $EN
[listen]
tcp = 127.0.75.$1:2475$1
[cluster]
multicast = 239.255.81.9:24750
advertise = 127.0.75.$1
pull_timeout_ms = 400
mode = store
collections = c
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
cli, sec, en = sys.argv[1], sys.argv[2], sys.argv[3]
def node(n): return ("127.0.75.%d" % n, "2475%d" % n)
M, A, B = node(9), node(1), node(2)
def call(n, req, priv=False):
    # perfcli prints the result alone on stdout, an error on stderr
    try:
        o = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"] + (["-E", en] if priv else []),
                           input=(json.dumps(req) + "\n").encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        out = o.stdout.decode().strip()
        return json.loads(out.splitlines()[0]) if out else {"_err": o.stderr.decode().strip() or "no reply"}
    except Exception as e:
        return {"_err": type(e).__name__}
def stats(n): r = call(n, {"method": "stats"}); return r.get("result", r)
def cl(n): return stats(n).get("cluster") or {}
def role(n): return cl(n).get("role")
def members(n): r = call(n, {"method": "members"}); r = r.get("result", r); return r.get("members") or []
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def err(r): return str(r.get("_err", ""))[:70]
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.2)
    return -1

until(lambda: role(M) == "master" and role(A) == "member" and role(B) == "member"
      and len(members(A)) == 3 and len(members(B)) == 3, 40)
out("ROLES0", "%s/%s/%s" % (role(M), role(A), role(B)))
idM, idA, idB = cl(M).get("node"), cl(A).get("node"), cl(B).get("node")
term0 = (cl(M).get("map") or {}).get("term")
call(M, {"method": "set", "params": {"col": "c", "key": "before", "value": "v0"}})
time.sleep(0.5)

out("NOPRIV", err(call(M, {"method": "master_prefer", "params": {"node": idA}})))
out("ONMEMBER", err(call(A, {"method": "master_prefer", "params": {"node": idB}}, True)))
out("NOTMEMBER", err(call(M, {"method": "master_prefer", "params": {"node": 999}}, True)))
out("STILL_MASTER", role(M))

t0 = time.time()
r = call(M, {"method": "master_prefer", "params": {"node": idA}}, True)
out("ASKED", r.get("handover_to") == idA)
gap = gap_max = 0.0; last = time.time(); double = samples = 0; done = -1
while time.time() - t0 < 15:
    rs = {n: cl(n) for n in (M, A, B)}
    now = time.time(); samples += 1
    nm = [n for n in rs if rs[n].get("role") == "master"]
    if len(nm) > 1: double += 1
    if not nm:
        gap += now - last; gap_max = max(gap_max, gap)
    else:
        gap = 0.0
    last = now
    if (rs[A].get("role"), rs[M].get("role"), rs[B].get("role")) == ("master", "member", "member") and \
            len(set(rs[n].get("master") for n in rs)) == 1 and rs[B].get("master") == idA:
        done = now - t0
        break
    time.sleep(0.1)
out("HANDOVER_MS", int(done * 1000) if done >= 0 else -1)
out("GAP_MS", int(gap_max * 1000)); out("DOUBLE", "%d of %d" % (double, samples))
cA, cM = cl(A), cl(M)
out("TERMS", "%s %s" % (term0, (cA.get("map") or {}).get("term")))
out("DEMOTIONS", cM.get("demotions"))
vals = []
call(A, {"method": "set", "params": {"col": "c", "key": "after", "value": "v1"}})
until(lambda: all(call(n, {"method": "get", "params": {"col": "c", "key": "after"}}).get("value") == "v1" for n in (M, A, B)), 10)
for k in ("before", "after"):
    for n in (M, A, B):
        vals.append(call(n, {"method": "get", "params": {"col": "c", "key": k}}).get("value"))
out("READS", "/".join(str(v) for v in vals))
r = call(A, {"method": "master_auto"}, True)
out("AUTO", r.get("handover_to"))
EOF
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

for n in 9 1 2; do conf $n; done
"$BIN" -f "$D/n9.conf" > "$D/n9.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n9.log" || { bad "the founder did not start"; echo "handovertest: $pass passed, $fail failed"; exit 1; }
sleep 1
for n in 1 2; do "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 & PIDS="$PIDS $!"; done
wait_ready "$D/n1.log" && wait_ready "$D/n2.log" || { bad "the members did not start"; echo "handovertest: $pass passed, $fail failed"; exit 1; }
python3 "$DRV" "$CLI" "$SEC" "$EN" > "$D/out" 2>&1

[ "$(val ROLES0)" = "master/member/member" ] && ok "three nodes: the founder (highest address) is master, two members (the control)" \
	|| bad "the fleet did not form as expected: $(val ROLES0) $(tail -1 "$D/out")"
[ -n "$(val NOPRIV)" ] && [ -n "$(val ONMEMBER)" ] && [ -n "$(val NOTMEMBER)" ] && [ "$(val STILL_MASTER)" = master ] \
	&& ok "refused without the enable secret ($(val NOPRIV | cut -c1-40)), on a member ($(val ONMEMBER | cut -c1-40)) and for node 999 - the master stays" \
	|| bad "the refusals: noprivilege '$(val NOPRIV)', on a member '$(val ONMEMBER)', node 999 '$(val NOTMEMBER)', master now $(val STILL_MASTER)"
H=$(num "$(val HANDOVER_MS)")
[ "$(val ASKED)" = True ] && [ "$H" -ge 0 ] && [ "$H" -le 8000 ] \
	&& ok "master_prefer: the named member is master in $H ms, the old master and the third node follow it" \
	|| bad "the handover: asked $(val ASKED), done after $(val HANDOVER_MS) ms"
G=$(num "$(val GAP_MS)")
[ "$H" -ge 0 ] && [ "$G" -ge 0 ] && [ "$G" -le 1500 ] \
	&& ok "the fleet was without a master for at most $G ms of samples (double: $(val DOUBLE)) - no election's silence" \
	|| bad "masterless for $(val GAP_MS) ms (double: $(val DOUBLE))"
T=$(val TERMS)
[ "$(num "${T##* }")" -gt "$(num "${T%% *}")" ] && [ "$(val DEMOTIONS)" = 1 ] \
	&& ok "the new master holds a later term ($T) and the old one counts one demotion" \
	|| bad "terms $T, old master's demotions $(val DEMOTIONS)"
grep -q "handing mastership to node" "$D/n9.log" && grep -q "hands mastership to this node" "$D/n1.log" \
	&& ok "both logs name the handover: $(grep -m1 'handing mastership' "$D/n9.log" | sed 's/.*cluster: //' | cut -c1-60)" \
	|| bad "the logs do not name the handover: $(grep -m1 'master' "$D/n9.log" | cut -c1-100)"
[ "$(val READS)" = "v0/v0/v0/v1/v1/v1" ] && ok "a key written before reads back on all three, and a write through the new master lands everywhere" \
	|| bad "reads: $(val READS)"
[ "$(val AUTO)" = 0 ] && ok "master_auto answers on the new master (nothing pending)" || bad "master_auto: $(val AUTO) $(tail -1 "$D/out")"

echo "handovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
