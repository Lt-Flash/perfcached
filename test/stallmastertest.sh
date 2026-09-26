#!/bin/sh
# stallmastertest.sh - S218: a master whose cluster thread has wedged is
# replaced, and yields when it comes back.
#
# S216 made a wedged node SAY stalled.  A wedged MASTER was still the
# master: the heartbeat watchdog re-sent its MASTER_ALIVE as faithfully as
# its ALIVE, so members saw a live master, nobody elected, and nothing
# only a master does happened - no joiner admitted, no map issued, no
# re-shard.  And withholding the claim is not enough on its own: the
# election defers to the highest LIVE address, a stalled node's watchdog
# keeps it live, and when the wedged master held the highest address every
# member deferred to it for ever.
#
# Three nodes; the master (the founder, and by address the one every
# election would defer to) has its cluster thread frozen for 24 s with
# apply_stall_ms = 2000, using test/threadfreeze.c:
#  - the kernel shows that one thread stopped and pc-beat running;
#  - a NEW master appears among the other two within apply_stall_ms +
#    MASTER_DEAD_MS (8 s) + a margin, and both agree on who it is;
#  - a fourth node started while the old master is still frozen is
#    admitted (it reaches ready and lists the fleet);
#  - the frozen node is listed as stalled by the new master, still a
#    member (nothing purged it), and still answers a read;
#  - at no sample are two RUNNING nodes both master;
#  - released, the old master yields (role member, one demotion counted),
#    the fleet has exactly one master, and a key the old master held
#    before the freeze reads back everywhere.
# Fail-first: on a daemon without S218 the frozen node stays master for
# the whole freeze and the joiner never gets in.
# Usage: test/stallmastertest.sh [./perfcached] [./threadfreeze] [./perfcli]
set -u
BIN=${1:-./perfcached}
TF=${2:-./threadfreeze}
CLI=${3:-./perfcli}
SEC=sm-client-secret
D=$(mktemp -d /var/tmp/pcsm.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$TF" ] || { echo "stallmastertest: $TF is missing (make threadfreeze)"; exit 1; }
[ -x "$CLI" ] || { echo "stallmastertest: $CLI is missing (make perfcli)"; exit 1; }

# node <n>: 127.0.70.<n>:1846<n>.  The master-to-be gets the HIGHEST
# address (9): with the founder frozen, every election would defer to it.
conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = sm-cluster-secret
[listen]
tcp = 127.0.70.$1:1846$1
[cluster]
multicast = 239.255.77.230:17430
advertise = 127.0.70.$1
pull_timeout_ms = 400
mode = store
collections = c
apply_stall_ms = 2000
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
import json, os, subprocess, sys, time
cli, sec, ctl, logdir = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
def node(n): return ("127.0.70.%d" % n, "1846%d" % n)
M, A, B, J = node(9), node(1), node(2), node(3)
FREEZE = 24000
def call(n, req):
    try:
        o = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"], input=(json.dumps(req) + "\n").encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout.decode()
        return json.loads(o.splitlines()[0])
    except Exception as e:
        return {"_err": type(e).__name__}
def stats(n): r = call(n, {"method": "stats"}); return r.get("result", r)
def role(n): return (stats(n).get("cluster") or {}).get("role")
def master_of(n): return (stats(n).get("cluster") or {}).get("master")
def members(n): r = call(n, {"method": "members"}); r = r.get("result", r); return r.get("members") or []
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.2)
    return -1
def tstate(tid):
    try: return open("/proc/%s/stat" % tid).read().rsplit(")", 1)[1].split()[0]
    except Exception: return "?"
def comm_tid(pid, comm):
    for t in os.listdir("/proc/%s/task" % pid):
        try:
            if open("/proc/%s/task/%s/comm" % (pid, t)).read().strip() == comm: return t
        except Exception: pass
    return None

until(lambda: role(M) == "master" and role(A) == "member" and role(B) == "member"
      and len(members(A)) == 3 and len(members(B)) == 3, 40)
out("ROLES0", "%s/%s/%s" % (role(M), role(A), role(B)))
call(M, {"method": "set", "params": {"col": "c", "key": "held-by-master", "value": "vm"}})
time.sleep(0.5)

t0 = time.time()
open(ctl, "w").write("1 pc-cluster %d\n" % FREEZE)
until(lambda: os.path.exists(ctl + ".ack") and open(ctl + ".ack").read().split()[1:2] == ["frozen"], 5)
ack = open(ctl + ".ack").read().split() if os.path.exists(ctl + ".ack") else []
ftid = ack[2] if len(ack) > 2 else ""
try: pid = open("/proc/%s/status" % ftid).read().split("Tgid:")[1].split()[0]
except Exception: pid = None
beat = comm_tid(pid, "pc-beat") if pid else None
out("PREMISE", "ok" if tstate(ftid) == "t" and beat and tstate(beat) in ("S", "R", "D") else "no (%s/%s)" % (tstate(ftid), tstate(beat) if beat else "?"))

# sample the RUNNING nodes until a new master appears; count double masters
double = 0; samples = 0; newm = None; t_new = -1
while time.time() - t0 < 22:
    r = {n: role(n) for n in (A, B)}
    samples += 1
    if list(r.values()).count("master") > 1: double += 1
    if newm is None and "master" in r.values():
        newm = [n for n in r if r[n] == "master"][0]; t_new = int((time.time() - t0) * 1000)
        break
    time.sleep(0.4)
out("NEW_MASTER_AFTER_MS", t_new); out("NEW_MASTER", newm[0] if newm else "none")
other = B if newm == A else A
until(lambda: master_of(A) == master_of(B) and master_of(A) is not None and role(other) == "member", 6)
out("AGREE", "A says %s, B says %s; %s is member" % (master_of(A), master_of(B), "other" if role(other) == "member" else other[0] + "=" + str(role(other))))
out("OLD_LISTED", json.dumps([(m.get("state"), m.get("reason", "")[:40], m.get("gone_s")) for m in members(newm or A) if not m.get("self") and m.get("state") not in ("ready",)]))
r = call(M, {"method": "get", "params": {"col": "c", "key": "held-by-master"}}); r = r.get("result", r)
out("OLD_READ", r.get("value"))

# a joiner while the old master is still frozen
open(logdir + "/JOIN", "w").write("go\n")
tj = until(lambda: stats(J).get("state") == "ready" and len(members(J)) == 4, 20)
out("JOINER_READY_AFTER_MS", int(tj * 1000) if tj >= 0 else -1)
out("JOINER_SEES", len(members(J)))
out("STILL_FROZEN", tstate(ftid))
while time.time() - t0 < FREEZE / 1000.0 + 1:
    r = {n: role(n) for n in (A, B, J)}
    samples += 1
    if list(r.values()).count("master") > 1: double += 1
    time.sleep(0.5)
out("DOUBLE", "%d of %d samples" % (double, samples))

until(lambda: os.path.exists(ctl + ".ack") and open(ctl + ".ack").read().split()[1:2] == ["released"], 10)
ty = until(lambda: role(M) == "member", 15)
out("OLD_YIELDED_AFTER_MS", int(ty * 1000) if ty >= 0 else -1)
out("OLD_YIELD_LINE", subprocess.run(["grep", "-m1", "stepping down\\|yielding mastership", logdir + "/n9.log"], stdout=subprocess.PIPE).stdout.decode().strip()[-100:])
out("OLD_DEMOTIONS", (stats(M).get("cluster") or {}).get("demotions"))
until(lambda: stats(M).get("state") == "ready", 8)
roles = [role(n) for n in (M, A, B, J)]
out("ROLES2", "/".join(str(x) for x in roles)); out("MASTERS2", roles.count("master"))
out("MASTER_AGREED2", len(set(master_of(n) for n in (M, A, B, J))))
vals = []
for n in (M, A, B, J):
    r = call(n, {"method": "get", "params": {"col": "c", "key": "held-by-master"}}); r = r.get("result", r); vals.append(r.get("value"))
out("READS2", "/".join(str(v) for v in vals))
EOF
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

for n in 9 1 2 3; do conf $n; done
: > "$D/ctl"
"$TF" "$D/ctl" -- "$BIN" -f "$D/n9.conf" > "$D/n9.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n9.log" || { bad "the founder did not start"; echo "stallmastertest: $pass passed, $fail failed"; exit 1; }
sleep 1
for n in 1 2; do "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 & PIDS="$PIDS $!"; done
wait_ready "$D/n1.log" && wait_ready "$D/n2.log" || { bad "the members did not start"; echo "stallmastertest: $pass passed, $fail failed"; exit 1; }
# the joiner starts on the driver's cue
( i=0; while [ $i -lt 600 ]; do [ -f "$D/JOIN" ] && break; sleep 0.1; i=$((i + 1)); done
  [ -f "$D/JOIN" ] && exec "$BIN" -f "$D/n3.conf" > "$D/n3.log" 2>&1 ) &
PIDS="$PIDS $!"
python3 "$DRV" "$CLI" "$SEC" "$D/ctl" "$D" > "$D/out" 2>&1

[ "$(val ROLES0)" = "master/member/member" ] && ok "three nodes: the founder (highest address) is master, two members (the control)" \
	|| bad "the fleet did not form as expected: $(val ROLES0) $(tail -1 "$D/out")"
[ "$(val PREMISE)" = ok ] && ok "the master's pc-cluster is stopped and its pc-beat runs - one thread wedged, the process alive" \
	|| bad "the premise failed: $(val PREMISE)"
T=$(num "$(val NEW_MASTER_AFTER_MS)")
[ "$T" -ge 1 ] && [ "$T" -le 16000 ] && ok "a new master (node at .$(val NEW_MASTER)) $T ms after the freeze (limit 2 s + MASTER_DEAD 8 s + margin)" \
	|| bad "no new master within 22 s ($(val NEW_MASTER_AFTER_MS)) - the fleet is held to a wedged master"
case "$(val AGREE)" in *"other is member"*) A1=$(val AGREE | sed 's/A says \([0-9]*\), B says \([0-9]*\).*/\1 \2/'); [ "${A1%% *}" = "${A1##* }" ] \
	&& ok "both running members agree on it ($(val AGREE))" || bad "the members disagree: $(val AGREE)";;
*) bad "not one master among the running nodes: $(val AGREE)";; esac
case "$(val OLD_LISTED)" in *'"stalled", "its cluster thread'*)
	case "$(val OLD_LISTED)" in *", -1]"*) ok "the new master lists the old one as stalled, with the reason, still a member ($(val OLD_LISTED | cut -c1-60)...)";;
	*) bad "the old master was purged: $(val OLD_LISTED)";; esac;;
*) bad "the new master's view of the old one: $(val OLD_LISTED)";; esac
[ "$(val OLD_READ)" = vm ] && ok "the frozen node still answers a read for a key it holds" || bad "the frozen node answered '$(val OLD_READ)'"
J=$(num "$(val JOINER_READY_AFTER_MS)")
[ "$J" -ge 0 ] && [ "$(val JOINER_SEES)" = 4 ] && [ "$(val STILL_FROZEN)" = t ] \
	&& ok "a fourth node started meanwhile was admitted in $J ms and lists all four - while the old master is still frozen (state t)" \
	|| bad "the joiner: ready after $(val JOINER_READY_AFTER_MS) ms, sees $(val JOINER_SEES), old master state '$(val STILL_FROZEN)'"
case "$(val DOUBLE)" in "0 of "*) ok "at no sample were two running nodes both master ($(val DOUBLE))";; *) bad "double master: $(val DOUBLE)";; esac
Y=$(num "$(val OLD_YIELDED_AFTER_MS)")
[ "$Y" -ge 0 ] && [ "$(val OLD_DEMOTIONS)" = 1 ] && ok "released, the old master yielded in $Y ms (1 demotion): $(val OLD_YIELD_LINE | sed 's/.*cluster: //' | cut -c1-70)" \
	|| bad "the old master after release: yielded after $(val OLD_YIELDED_AFTER_MS) ms, demotions $(val OLD_DEMOTIONS)"
[ "$(val MASTERS2)" = 1 ] && [ "$(val MASTER_AGREED2)" = 1 ] && ok "the fleet has exactly one master and all four agree on it ($(val ROLES2))" \
	|| bad "after release: roles $(val ROLES2), $(val MASTER_AGREED2) distinct master id(s)"
[ "$(val READS2)" = "vm/vm/vm/vm" ] && ok "and the key the old master held before the freeze reads back on all four" \
	|| bad "reads after release: $(val READS2)"

echo "stallmastertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
