#!/bin/sh
# stalltest.sh - S216: a node whose cluster thread has wedged says so.
#
# ONE thread applies every replicated record, answers every pull and runs
# the control plane; ANOTHER keeps the heartbeat going when the first is
# heads-down, by re-sending the last frame it built.  So a cluster thread
# that blocks for good leaves a node that goes on heartbeating READY,
# stays in every client's selection, and serves nothing on the cluster
# plane - for ever.  Nothing told anybody.
#
# The fault is made with test/threadfreeze.c: the daemon's parent stops
# exactly ONE of its threads (pc-cluster) with ptrace; the heartbeat
# thread and the workers go on running, which is the whole point.
#
# A two-node fleet, node B under the freezer, apply_stall_ms = 2000:
#  - B is READY and the fleet replicates (the control);
#  - the thread is frozen for 9 s - CONFIRMED by the kernel: that task
#    reads "t" (tracing stop) in /proc while pc-beat of the same process
#    does not;
#  - B says STALLED within the limit plus a beat, with a reason that
#    names the cluster thread, in its own stats;
#  - A - which hears it only from B's heartbeat thread - lists B as
#    stalled with that reason, and B never stops being a MEMBER (no
#    purge, no rejoin: nothing re-shards over a stall);
#  - B still answers a read for a key it holds;
#  - released, B is READY again by itself after a steady apply_stall_ms,
#    A sees it, and the stall is counted with its length.
# A third arm, the false positive that must not happen: the WHOLE process
# stopped (SIGSTOP, as a VM pause would) for longer than the limit, then
# continued - every thread's clock is old for the same reason, and that is
# not a wedged thread: B must not report a stall.
# A second arm, apply_stall_ms = 0: the same freeze, B stays READY - the
# detector is off when it is switched off.
# STALLTEST_LEGACY=1 omits the key and freezes past the 10 s default, so
# the suite can be pointed at a daemon from before the detector.
# Usage: test/stalltest.sh [./perfcached] [./threadfreeze] [./perfcli]
set -u
BIN=${1:-./perfcached}
TF=${2:-./threadfreeze}
CLI=${3:-./perfcli}
SEC=st-client-secret
D=$(mktemp -d /var/tmp/pcst.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

[ -x "$TF" ] || { echo "stalltest: $TF is missing (make threadfreeze) - there is nothing to wedge a thread with"; exit 1; }
[ -x "$CLI" ] || { echo "stalltest: $CLI is missing (make perfcli)"; exit 1; }

LIMIT=2000; FREEZE=9000
[ "${STALLTEST_LEGACY:-0}" = 1 ] && { LIMIT=10000; FREEZE=14000; }

# conf <arm> <node> <stall-line>: 127.0.69.<arm><node>:1845<node>, group .21<6+arm>
conf() {
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = st-cluster-secret
[listen]
tcp = 127.0.69.$1$2:1845$2
[cluster]
multicast = 239.255.77.22$1:1742$1
advertise = 127.0.69.$1$2
pull_timeout_ms = 400
mode = store
collections = c
$3
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1$2.conf"
}
wait_ready() { # <log>
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$1" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "no daemon: $(tail -2 "$1" | tr '\n' ' ')"; return 1
}

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# drv.py <cli> <secret> <arm> <limit_ms> <freeze_ms> <ctl>: drives one arm
import json, subprocess, sys, time
cli, sec, arm, limit, freeze, ctl = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
A = ("127.0.69.%s1" % arm, "18451"); B = ("127.0.69.%s2" % arm, "18452")
def call(n, req):
    try:
        o = subprocess.run([cli, "-h", n[0], "-p", n[1], "-a", sec, "-q"], input=(json.dumps(req) + "\n").encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout.decode()
        return json.loads(o.splitlines()[0])
    except Exception as e:
        return {"_err": type(e).__name__}
def stats(n): r = call(n, {"method": "stats"}); return r.get("result", r)
def b_at_a():
    r = call(A, {"method": "members"}); r = r.get("result", r)
    for m in r.get("members") or []:
        if not m.get("self"): return m
    return {}
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def until(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return time.time() - t
        time.sleep(0.1)
    return -1

until(lambda: stats(A).get("state") == "ready" and stats(B).get("state") == "ready" and b_at_a().get("state") == "ready", 40)
out("STATE0", "%s/%s, A sees B %s" % (stats(A).get("state"), stats(B).get("state"), b_at_a().get("state")))
call(B, {"method": "set", "params": {"col": "c", "key": "held-by-b", "value": "vb"}})
call(A, {"method": "set", "params": {"col": "c", "key": "held-by-a", "value": "va"}})
r = call(B, {"method": "get", "params": {"col": "c", "key": "held-by-a"}}); r = r.get("result", r)
out("PULL0", r.get("value"))

def tstate(tid):                                   # the letter in /proc/<tid>/stat: t = tracing stop
    try: return open("/proc/%s/stat" % tid).read().rsplit(")", 1)[1].split()[0]
    except Exception: return "?"
def comm_tid(pid, comm):
    import os
    for t in os.listdir("/proc/%s/task" % pid):
        try:
            if open("/proc/%s/task/%s/comm" % (pid, t)).read().strip() == comm: return t
        except Exception: pass
    return None
t0 = time.time()
open(ctl, "w").write("1 pc-cluster %d\n" % freeze)
until(lambda: open(ctl + ".ack").read().split()[1:2] == ["frozen"] if __import__("os").path.exists(ctl + ".ack") else False, 5)
out("ACK1", open(ctl + ".ack").read().strip() if __import__("os").path.exists(ctl + ".ack") else "none")
# the premise, read off the kernel AT ONCE (the freeze is finite): THAT thread is stopped, and the
# heartbeat thread of the same process is not
ftid = (open(ctl + ".ack").read().split() + ["", "", ""])[2]
try: pid = open("/proc/%s/status" % ftid).read().split("Tgid:")[1].split()[0]
except Exception: pid = None
beat = comm_tid(pid, "pc-beat") if pid else None
out("FROZEN_THREAD", "pc-cluster tid %s state %s" % (ftid, tstate(ftid)))
out("BEAT_THREAD", "pc-beat tid %s state %s" % (beat, tstate(beat) if beat else "?"))
out("PREMISE", "ok" if tstate(ftid) == "t" and beat and tstate(beat) in ("S", "R", "D") else "no")

tb = until(lambda: stats(B).get("state") == "stalled", (limit + 6000) / 1000.0)
out("B_STALLED_AFTER_MS", int((time.time() - t0) * 1000) if tb >= 0 else -1)
sb = stats(B); cb = sb.get("cluster") or {}
out("B_STATE1", sb.get("state")); out("B_REASON1", sb.get("state_reason")); out("B_FLAG1", cb.get("stalled"))
ta = until(lambda: b_at_a().get("state") == "stalled", 5)
m = b_at_a()
out("A_SEES1", m.get("state")); out("A_REASON1", m.get("reason")); out("A_GONE1", m.get("gone_s"))
out("A_SEES_AFTER_MS", int((time.time() - t0) * 1000) if ta >= 0 else -1)
r = call(B, {"method": "get", "params": {"col": "c", "key": "held-by-b"}}); r = r.get("result", r)
out("B_READ1", r.get("value"))

until(lambda: open(ctl + ".ack").read().split()[1:2] == ["released"], freeze / 1000.0 + 5)
t1 = time.time()
out("A_MEMBERS_AT_RELEASE", b_at_a().get("state"))
tr = until(lambda: stats(B).get("state") == "ready", (limit + 6000) / 1000.0)
out("B_READY_AFTER_MS", int((time.time() - t1) * 1000) if tr >= 0 else -1)
until(lambda: b_at_a().get("state") == "ready", 5)
out("A_SEES2", b_at_a().get("state"))
cb = stats(B).get("cluster") or {}
out("STALLS", cb.get("apply_stalls")); out("STALL_MAX_MS", cb.get("apply_stall_ms_max")); out("B_FLAG2", cb.get("stalled"))
r = call(B, {"method": "get", "params": {"col": "c", "key": "held-by-a"}}); r = r.get("result", r)
out("PULL2", r.get("value"))
EOF
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

arm() { # arm <n> <stall-line>
	conf $1 1 "$2"; conf $1 2 "$2"
	"$BIN" -f "$D/n${1}1.conf" > "$D/n${1}1.log" 2>&1 &
	PIDS="$PIDS $!"
	: > "$D/ctl$1"
	"$TF" "$D/ctl$1" -- "$BIN" -f "$D/n${1}2.conf" > "$D/n${1}2.log" 2>&1 &
	PIDS="$PIDS $!"
	wait_ready "$D/n${1}1.log" && wait_ready "$D/n${1}2.log" || return 1
	python3 "$DRV" "$CLI" "$SEC" $1 $LIMIT $FREEZE "$D/ctl$1" > "$D/$1.out" 2>&1
}
stop_arm() { for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.6
	for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; }

# ---- the detector on ----------------------------------------------------
echo "--- apply_stall_ms = $LIMIT, pc-cluster frozen for $FREEZE ms"
KEY="apply_stall_ms = $LIMIT"; [ "${STALLTEST_LEGACY:-0}" = 1 ] && KEY=""
if arm 1 "$KEY"; then
	case "$(val 1 STATE0)" in "ready/ready, A sees B ready")
		[ "$(val 1 PULL0)" = va ] && ok "two nodes READY, and B pulls a key A holds - the cluster plane works (the control)" \
			|| bad "control: B could not pull a key from A ($(val 1 PULL0))";;
	*) bad "control: the fleet did not form: $(val 1 STATE0) $(tail -1 "$D/1.out")";; esac
	case "$(val 1 ACK1)" in *" frozen "*)
		[ "$(val 1 PREMISE)" = ok ] && ok "the kernel agrees: $(val 1 FROZEN_THREAD) (t = stopped), $(val 1 BEAT_THREAD) - one thread has the fault, the process does not" \
			|| bad "the freezer acked but the kernel shows $(val 1 FROZEN_THREAD), $(val 1 BEAT_THREAD)";;
	*) bad "the thread was not frozen: $(val 1 ACK1) - nothing below is evidence";; esac
	T=$(num "$(val 1 B_STALLED_AFTER_MS)")
	[ "$(val 1 B_STATE1)" = stalled ] && [ "$T" -ge $((LIMIT - 300)) ] && [ "$T" -le $((LIMIT + 2500)) ] \
		&& ok "B says STALLED $T ms after the freeze (limit $LIMIT + a beat)" \
		|| bad "B is '$(val 1 B_STATE1)' (stalled after $(val 1 B_STALLED_AFTER_MS) ms; want ~$LIMIT) - a wedged cluster thread and a healthy-looking node"
	case "$(val 1 B_REASON1)" in *"cluster thread"*) [ "$(val 1 B_FLAG1)" = True ] \
		&& ok "and why: $(val 1 B_REASON1 | cut -c1-70)..." || bad "cluster.stalled is $(val 1 B_FLAG1)";;
	*) bad "B's reason reads: '$(val 1 B_REASON1)'";; esac
	case "$(val 1 A_SEES1)|$(val 1 A_REASON1)" in "stalled|"*"cluster thread"*)
		ok "A lists B as stalled with the reason ($(val 1 A_SEES_AFTER_MS) ms after the freeze) - told by B's heartbeat thread, the only one running";;
	*) bad "A lists B as '$(val 1 A_SEES1)' ('$(val 1 A_REASON1)') - the fleet was not told";; esac
	[ "$(val 1 A_GONE1)" = -1 ] && ! grep -q "purg\|went away\|lost peer" "$D/n11.log" \
		&& ok "and B never stopped being a member: no purge, no rejoin - nothing re-shards over a stall" \
		|| bad "A treated the stall as a departure (gone_s $(val 1 A_GONE1)): $(grep "purg\|went away\|lost peer" "$D/n11.log" | tail -1 | cut -c1-120)"
	[ "$(val 1 B_READ1)" = vb ] && ok "B still answers a read for a key it holds - STALLED refuses nothing" \
		|| bad "B answered '$(val 1 B_READ1)' for its own key while stalled"
	R=$(num "$(val 1 B_READY_AFTER_MS)")
	[ "$R" -ge $((LIMIT - 600)) ] && [ "$R" -le $((LIMIT + 3000)) ] && [ "$(val 1 A_SEES2)" = ready ] && [ "$(val 1 B_FLAG2)" = False ] \
		&& ok "released, B is READY again by itself after a steady $R ms (not at once: a limping thread must not flap), and A sees it" \
		|| bad "after release: B ready after $(val 1 B_READY_AFTER_MS) ms (want ~$LIMIT), A sees '$(val 1 A_SEES2)', flag $(val 1 B_FLAG2)"
	M=$(num "$(val 1 STALL_MAX_MS)")
	[ "$(val 1 STALLS)" = 1 ] && [ "$M" -ge $((FREEZE - 1500)) ] && [ "$M" -le $((FREEZE + 3000)) ] \
		&& ok "the stall is counted once, $M ms long (frozen $FREEZE)" \
		|| bad "apply_stalls $(val 1 STALLS), apply_stall_ms_max $(val 1 STALL_MAX_MS) (frozen $FREEZE)"
	grep -q "WARNING.*made no progress for" "$D/n12.log" && grep -q "NOTICE.*is moving again" "$D/n12.log" \
		&& ok "B's log: $(grep -m1 "made no progress" "$D/n12.log" | sed 's/.*cluster: //' | cut -c1-95)..." \
		|| bad "B's log has no stall lines: $(grep -c "progress\|moving again" "$D/n12.log")"
	[ "$(val 1 PULL2)" = va ] && ok "and the cluster plane works again (B pulls from A)" || bad "after the stall B cannot pull: $(val 1 PULL2)"
else
	bad "arm 1 did not start"
fi
stop_arm

# ---- the detector off ---------------------------------------------------
if [ "${STALLTEST_LEGACY:-0}" != 1 ]; then
	echo "--- apply_stall_ms = 0, the same freeze for 4000 ms"
	FREEZE=4000; LIMIT=2000
	if arm 2 "apply_stall_ms = 0"; then
		case "$(val 2 ACK1)" in *" frozen "*)
			[ "$(val 2 B_STATE1)" = ready ] && [ "$(val 2 STALLS)" = 0 ] \
				&& ok "switched off, a frozen cluster thread changes nothing: B stays READY, 0 stalls counted" \
				|| bad "apply_stall_ms = 0 and B is '$(val 2 B_STATE1)', stalls $(val 2 STALLS)";;
		*) bad "arm 2: the thread was not frozen: $(val 2 ACK1)";; esac
	else
		bad "arm 2 did not start"
	fi
	stop_arm
fi

# ---- the whole process paused is not a wedged thread ---------------------
# Built so that ONLY the guard can pass it.  A plain STOP/CONT proves
# nothing: on CONT the cluster thread wakes and stamps its clock before
# the heartbeat thread looks, guard or no guard (tried: the arm stayed
# green with the guard disabled).  So the thread is ALSO frozen, from just
# before the stop until 0.4 s after the continue: when the process
# resumes the clock is 4.6 s old and nothing has stamped it, yet the
# thread itself was unable to run for only ~0.7 s of the process's own
# time - under the 2 s limit.
if [ "${STALLTEST_LEGACY:-0}" != 1 ]; then
	echo "--- apply_stall_ms = 2000, the WHOLE daemon stopped (SIGSTOP) for 4 s, its cluster thread frozen across the resume"
	conf 3 1 "apply_stall_ms = 2000"; conf 3 2 "apply_stall_ms = 2000"
	"$BIN" -f "$D/n31.conf" > "$D/n31.log" 2>&1 &
	PIDS="$PIDS $!"
	: > "$D/ctl3"
	"$TF" "$D/ctl3" -- "$BIN" -f "$D/n32.conf" > "$D/n32.log" 2>&1 &
	TFP=$!; PIDS="$PIDS $TFP"
	if wait_ready "$D/n31.log" && wait_ready "$D/n32.log"; then
		sleep 3
		BP=$(pgrep -P $TFP | head -1)
		echo "1 pc-cluster 4700" > "$D/ctl3"
		i=0; while [ $i -lt 50 ]; do grep -q " frozen " "$D/ctl3.ack" 2>/dev/null && break; sleep 0.1; i=$((i + 1)); done
		sleep 0.2
		kill -STOP $BP; sleep 4
		ST=$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' /proc/$BP/status)
		kill -CONT $BP
		i=0; while [ $i -lt 50 ]; do grep -q " released " "$D/ctl3.ack" 2>/dev/null && break; sleep 0.1; i=$((i + 1)); done
		REL=$(cat "$D/ctl3.ack" 2>/dev/null)
		sleep 3
		R=$(printf '%s\n' '{"method":"stats"}' | timeout 15 "$CLI" -h 127.0.69.32 -p 18452 -a $SEC -q 2>/dev/null | head -1 \
			| python3 -c 'import json,sys
try:
    d=json.load(sys.stdin); d=d.get("result",d); c=d.get("cluster") or {}
    print(d.get("state"), c.get("apply_stalls"))
except Exception as e: print("ERR", type(e).__name__)')
		case "$REL" in *" released "*) ;; *) bad "arm 3: the freezer never released ($REL) - the sequence did not run";; esac
		[ "$ST" = T ] && [ "$R" = "ready 0" ] && ! grep -q "made no progress" "$D/n32.log" \
			&& ok "stopped whole (State T) for 4 s, the cluster thread frozen across the resume: B is ready, 0 stalls, no warning - a paused process is not a wedged thread" \
			|| bad "after a whole-process pause (State $ST): '$R', log: $(grep -c "made no progress" "$D/n32.log") stall line(s) - a pause was read as a stall"
	else
		bad "arm 3 did not start"
	fi
	stop_arm
fi

echo "stalltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
