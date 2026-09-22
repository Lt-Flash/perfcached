#!/bin/sh
# syncfailtest.sh - S215: a sync the device refused must not move the
# durable watermark, and must not be survived quietly.
#
# THE BUG (found by an outside read of the source, 2026-09-22, rc24):
# wal_fdatasync_timed() called fdatasync() and returned void, and four
# callers then wrote W.synced_seq = W.pending_hi - so on EIO the `sync`
# verb answered {"synced":true}, the metric and the page agreed, and the
# node went on as a healthy member.  The snapshot's directory fsync was
# unchecked too, and R.safe_marker moved behind it - the value the WAL
# consults before it overwrites a segment.
#
# The policy: the FIRST refused sync fails the node and is never retried
# on that descriptor - after a failed fsync Linux marks the pages clean,
# so the retry can return 0 having written nothing ("fsyncgate").
#
# The device here is test/syncfailshim.c, an LD_PRELOAD that refuses
# fdatasync/fsync by descriptor class and is armed at RUNTIME, so each
# daemon starts on a healthy device and does real work first.  Every arm
# asserts the fault was DELIVERED (the shim's own log) before it reads
# anything into what the daemon did about it.
#
#  always    standalone, fsync = always: the watermark stays where it
#            was, `sync` answers failure and promptly, the node is FAILED
#            and says why, the next write is refused, a read is served,
#            the log names the errno.  A daemon with no [cluster] must do
#            all of it - it has no cluster plane to be told by.
#  everysec  a one-node CLUSTER, fsync = everysec: the timer's sync is
#            refused; the same, through the cluster's callback.
#  append    the pwrite is refused, not the sync: this used to be
#            `W.enabled = 0` - the WAL stopped, the node carried on taking
#            writes, and `sync` said persistence was not configured.
#  ctrl      CONTROL's sync refused: logged with the errno and counted,
#            and the node stays READY - CONTROL is rewritten whole each
#            time, so the next sync covers all of it (DESIGN 12fi).
#  dir       the snapshot's directory sync refused: the snapshot is NOT
#            published - saves and safe_marker stay, save_errors moves,
#            the node stays READY - and with the device healthy again the
#            next snapshot publishes.
#  nodir     the directory cannot be opened at all: the same.
# Usage: test/syncfailtest.sh [./perfcached] [./syncfailshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
D=$(mktemp -d /var/tmp/pcsf.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

[ -f "$SHIM" ] || { echo "syncfailtest: $SHIM is missing (make syncfailshim.so) - there is no device to refuse anything"; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
# A daemon built with a DYNAMIC sanitizer runtime (gcc's check-asan)
# refuses to start unless that runtime is first in the preload list; a
# static one (clang) has nothing to order.  Either way the shim goes last.
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"

# conf <arm> <port> <fsync> <save-line> <segment_mb> <cluster 0|1>
conf() {
	cat > "$D/$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = sf-client-secret
cluster = sf-cluster-secret
[listen]
tcp = 127.0.68.1:$2
plaintext = loopback
[wal]
dir = $D/$1.wal
probe = no
fsync = $3
segment_mb = $5
segments = 4
ring_kb = 16384
$4
[collection c]
buckets_log2 = 12
EOF
	[ "$6" = 1 ] && cat >> "$D/$1.conf" <<EOF
[cluster]
multicast = 239.255.77.215:17415
advertise = 127.0.68.1
mode = store
collections = c
EOF
	mkdir -p "$D/$1.wal"
	: > "$D/$1.ctl"; : > "$D/$1.shim"
}
start() { # start <arm>
	PC_SYNCFAIL_CTL="$D/$1.ctl" PC_SYNCFAIL_LOG="$D/$1.shim" LD_PRELOAD="$PRELOAD" \
		"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 &
	P1=$!
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/$1.log" 2>/dev/null && return 0
		kill -0 $P1 2>/dev/null || break
		sleep 0.1; i=$((i + 1))
	done
	echo "arm $1: the daemon did not start:"; tail -4 "$D/$1.log"; return 1
}
stop() { [ -n "$P1" ] && kill -9 $P1 2>/dev/null; P1=; sleep 0.4; }

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# drv.py <arm> <port> <ctl-file>: drives one arm, prints KEY=VALUE lines
import json, socket, sys, time
arm, port, ctl = sys.argv[1], int(sys.argv[2]), sys.argv[3]
f = socket.create_connection(("127.0.68.1", port), timeout=30).makefile("rwb")
def call(m, **p):
    r = {"jsonrpc": "2.0", "id": 1, "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def stats(): return call("stats").get("result", {})
def put(k, v="v"): return call("set", col="c", key=k, value=v)
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " ")))
def set_ctl(s): open(ctl, "w").write(s)
def wait(pred, secs):
    t = time.time()
    while time.time() - t < secs:
        if pred(): return True
        time.sleep(0.1)
    return pred()

wait(lambda: stats().get("state") == "ready", 30)
out("STATE0", stats().get("state"))

if arm in ("always", "everysec", "append"):
    for i in range(20): put("pre%d" % i)
    r = call("sync", timeout_ms=4000)
    out("SYNC0", "ok" if (r.get("result") or {}).get("synced") else json.dumps(r)[:120])
    s0 = (stats().get("wal") or {}).get("synced_seq", -1)
    out("SYNCED0", s0)
    set_ctl("walwrite" if arm == "append" else "wal")
    for i in range(20): put("post%d" % i)          # acknowledged at the ring
    if arm != "always":
        wait(lambda: stats().get("state") == "failed", 4)   # the timer's sync, the next pump
    t = time.time()
    r = call("sync", timeout_ms=4000)
    out("SYNC1_MS", int((time.time() - t) * 1000))
    out("SYNC1", "SYNCED" if (r.get("result") or {}).get("synced") else "error:" + str((r.get("error") or {}).get("message"))[:100])
    wait(lambda: stats().get("state") == "failed", 3)
    st = stats()
    out("SYNCED1", (st.get("wal") or {}).get("synced_seq", -1))
    out("PENDING1", (st.get("wal") or {}).get("unsynced", -1))
    out("STATE1", st.get("state"))
    out("REASON1", st.get("state_reason"))
    r = put("after")
    out("WRITE1", "STORED" if (r.get("result") or {}).get("stored") else "refused:" + str((r.get("error") or {}).get("message"))[:60])
    r = call("get", col="c", key="pre3")
    out("READ1", (r.get("result") or {}).get("value"))
    # and it stays where it is: more time, another barrier, no movement
    time.sleep(1.3)
    call("sync", timeout_ms=500)
    out("SYNCED2", (stats().get("wal") or {}).get("synced_seq", -1))

elif arm == "ctrl":
    set_ctl("ctrl")
    v = "x" * 1000
    for i in range(2600): put("c%d" % i, v)        # ~2.7 MB through 1 MB segments
    call("sync", timeout_ms=4000)
    time.sleep(0.5)
    st = stats()
    out("STATE1", st.get("state"))
    out("CTRL_ERRORS", (st.get("wal") or {}).get("ctrl_errors", "absent"))
    out("RECYCLES", (st.get("wal") or {}).get("recycles", -1))

else:                                               # dir, nodir
    for i in range(200): put("a%d" % i)
    call("save")
    wait(lambda: (stats().get("rdb") or {}).get("saves") == 1, 10)
    r0 = stats().get("rdb") or {}
    out("SAVES0", r0.get("saves")); out("SAFE0", r0.get("safe_marker", "absent"))
    for i in range(200): put("b%d" % i)
    set_ctl(arm)
    call("save")
    wait(lambda: (stats().get("rdb") or {}).get("save_errors", 0) >= 1
         or (stats().get("rdb") or {}).get("saves") == 2, 10)
    time.sleep(0.5)
    st = stats(); r1 = st.get("rdb") or {}
    out("SAVES1", r1.get("saves")); out("SAFE1", r1.get("safe_marker", "absent"))
    out("ERRORS1", r1.get("save_errors", "absent")); out("LAST1", r1.get("last_marker"))
    out("STATE1", st.get("state"))
    set_ctl("")
    call("save")
    wait(lambda: (stats().get("rdb") or {}).get("saves") == 2, 10)
    r2 = stats().get("rdb") or {}
    out("SAVES2", r2.get("saves")); out("SAFE2", r2.get("safe_marker", "absent"))
EOF
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

# ---- the WAL's own sync refused ---------------------------------------
walarm() { # walarm <arm> <port> <fsync> <cluster> <what> <shim-class> <errno-text>
	conf $1 $2 $3 "save = off" 8 $4
	start $1 || { bad "$1: no daemon"; return; }
	python3 "$DRV" $1 $2 "$D/$1.ctl" > "$D/$1.out" 2>&1
	[ "$(val $1 STATE0)" = ready ] && [ "$(val $1 SYNC0)" = ok ] \
		&& ok "$5: ready, and on a healthy device a sync barrier is honoured (synced_seq $(val $1 SYNCED0))" \
		|| bad "$1: the control failed - state $(val $1 STATE0), sync $(val $1 SYNC0): $(tail -2 "$D/$1.out" | tr '\n' ' ')"
	N=$(grep -c "^$6 " "$D/$1.shim")
	[ "$N" -ge 1 ] && ok "the device refused the WAL ($6: $7) - the fault was delivered" \
		|| bad "$1: the shim refused nothing - every line below would be read off a healthy device"
	[ "$N" = 1 ] && ok "and was asked exactly ONCE: a refusal is never retried on that descriptor" \
		|| bad "the device was asked $N times - a retry after a refusal can succeed having written nothing"
	S0=$(num "$(val $1 SYNCED0)"); S1=$(num "$(val $1 SYNCED1)"); S2=$(num "$(val $1 SYNCED2)")
	[ "$S0" -ge 1 ] && [ "$S1" = "$S0" ] && [ "$S2" = "$S0" ] \
		&& ok "synced_seq stayed at $S0 - through the refusal, another second, and another barrier ($(val $1 PENDING1) record(s) honestly unsynced)" \
		|| bad "synced_seq moved over a refused sync: $S0 -> $S1 -> $S2"
	case "$(val $1 SYNC1)" in
	error:*) MS=$(num "$(val $1 SYNC1_MS)")
		[ "$MS" -ge 0 ] && [ "$MS" -lt 2500 ] && ok "the sync verb answers failure, in $MS ms - not after its 4 s timeout ($(val $1 SYNC1))" \
			|| bad "the sync verb failed only by timing out ($MS ms): $(val $1 SYNC1)";;
	*) bad "the sync verb answered '$(val $1 SYNC1)' over a device that refused the sync";;
	esac
	R=$(val $1 REASON1)
	[ "$(val $1 STATE1)" = failed ] && ok "the node is FAILED" || bad "the node is '$(val $1 STATE1)' after its WAL's sync was refused"
	case "$R" in
	''|*"without recording a reason"*) bad "FAILED, and the reason reads: '$R'";;
	*) ok "and says why: $R";;
	esac
	case "$(val $1 WRITE1)" in refused:*) ok "the next write is refused ($(val $1 WRITE1))";;
		*) bad "the next write was '$(val $1 WRITE1)' on a node that cannot log it";; esac
	[ "$(val $1 READ1)" = v ] && ok "a read is still served" || bad "a read of a key written before the fault answers '$(val $1 READ1)'"
	grep -q "CRIT.*wal: .*($7)" "$D/$1.log" \
		&& ok "the log names the errno: $(grep -m1 "($7)" "$D/$1.log" | sed 's/.*wal: //' | cut -c1-90)..." \
		|| bad "no CRIT line names the errno: $(grep -i "wal" "$D/$1.log" | tail -1 | cut -c1-120)"
	stop
}
echo "--- always: standalone, fsync = always"
walarm always 18431 always 0 "standalone, fsync = always" wal "Input/output error"
echo "--- everysec: one-node cluster, fsync = everysec"
walarm everysec 18432 everysec 1 "one-node cluster, fsync = everysec" wal "Input/output error"
echo "--- append: the append itself refused (ENOSPC)"
walarm append 18436 everysec 0 "standalone, a refused append" walwrite "No space left on device"

# ---- CONTROL ----------------------------------------------------------
echo "--- ctrl: CONTROL's sync refused"
conf ctrl 18433 everysec "save = off" 1 0
if start ctrl; then
	python3 "$DRV" ctrl 18433 "$D/ctrl.ctl" > "$D/ctrl.out" 2>&1
	N=$(grep -c "^ctrl " "$D/ctrl.shim")
	[ "$N" -ge 1 ] && ok "the device refused $N CONTROL sync(s) over $(val ctrl RECYCLES) recycle(s) - the fault was delivered" \
		|| bad "ctrl: the shim refused nothing (recycles $(val ctrl RECYCLES)) - CONTROL was never rewritten"
	E=$(num "$(val ctrl CTRL_ERRORS)")
	[ "$E" -ge 1 ] && grep -q "CONTROL.*Input/output error" "$D/ctrl.log" \
		&& ok "each is logged with its errno and counted (wal.ctrl_errors $E)" \
		|| bad "a refused CONTROL sync went unrecorded: ctrl_errors '$(val ctrl CTRL_ERRORS)', log: $(grep -c CONTROL "$D/ctrl.log") CONTROL line(s)"
	[ "$(val ctrl STATE1)" = ready ] && ok "and the node stays READY - CONTROL is rewritten whole, the next sync covers it" \
		|| bad "ctrl: the node is '$(val ctrl STATE1)'"
	stop
else
	bad "ctrl: no daemon"
fi

# ---- the snapshot's directory ------------------------------------------
dirarm() { # dirarm <arm> <port> <what>
	conf $1 $2 everysec "" 8 0
	start $1 || { bad "$1: no daemon"; return; }
	python3 "$DRV" $1 $2 "$D/$1.ctl" > "$D/$1.out" 2>&1
	[ "$(val $1 SAVES0)" = 1 ] && ok "$3: a first snapshot publishes on the healthy device (safe_marker $(val $1 SAFE0))" \
		|| bad "$1: the control failed - saves $(val $1 SAVES0): $(tail -2 "$D/$1.out" | tr '\n' ' ')"
	N=$(grep -c "^$1 " "$D/$1.shim")
	[ "$N" -ge 1 ] && ok "the fault was delivered ($N refusal(s): $(grep -m1 "^$1 " "$D/$1.shim" | cut -d' ' -f2 | sed "s|$D/||"))" \
		|| bad "$1: the shim refused nothing"
	[ "$(val $1 SAVES1)" = 1 ] && [ "$(num "$(val $1 ERRORS1)")" -ge 1 ] \
		&& ok "the second snapshot is NOT published: saves stays 1, save_errors $(val $1 ERRORS1)" \
		|| bad "a snapshot whose rename never became durable was published: saves $(val $1 SAVES0) -> $(val $1 SAVES1), save_errors $(val $1 ERRORS1)"
	A=$(num "$(val $1 SAFE0)"); B=$(num "$(val $1 SAFE1)"); L=$(num "$(val $1 LAST1)")
	[ "$A" -ge 1 ] && [ "$B" = "$A" ] && [ "$L" -gt "$A" ] \
		&& ok "safe_marker stays at $A while the attempt reached $L - the WAL keeps every segment the old marker protects" \
		|| bad "safe_marker: $(val $1 SAFE0) -> $(val $1 SAFE1) (the attempt's marker: $(val $1 LAST1))"
	[ "$(val $1 STATE1)" = ready ] && grep -q "rdb:.*\(Input/output error\|Permission denied\)" "$D/$1.log" \
		&& ok "the node stays READY - nothing acknowledged is lost while the WAL holds - and the log names the errno" \
		|| bad "$1: state '$(val $1 STATE1)', rdb log: $(grep "rdb:" "$D/$1.log" | tail -1 | cut -c1-120)"
	C=$(num "$(val $1 SAFE2)")
	[ "$(val $1 SAVES2)" = 2 ] && [ "$C" -gt "$A" ] \
		&& ok "with the device healthy again the next snapshot publishes (saves 2, safe_marker $C)" \
		|| bad "$1: the snapshot after the fault did not publish: saves $(val $1 SAVES2), safe_marker $(val $1 SAFE2)"
	stop
}
echo "--- dir: the snapshot's directory sync refused"
dirarm dir 18434 "directory sync"
echo "--- nodir: the snapshot's directory cannot be opened"
dirarm nodir 18435 "unopenable directory"

echo "syncfailtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
