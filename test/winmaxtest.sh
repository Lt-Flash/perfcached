#!/bin/sh
# winmaxtest.sh - S197: the window's MAX covers the same slots as the
# window's CALLS.
#
# w_calls is a difference against the oldest SNAPSHOT - the calls since that
# snapshot was taken - while w_max scanned every slot of the ring, including
# the slot that snapshot closed.  The two spans differed by one slot, so a
# verb last called six minutes ago reported win_calls=0 beside
# win_max_us=250: no calls in the window, and a slowest call in it.  The
# status page sorts on that column, so silent verbs sorted above busy ones
# on an all-time slowest call.
#
# The roll is once a minute (PC_OBS_WIN x 60 s), so this suite WAITS for one.
# Usage: test/winmaxtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwm.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = wm-client-secret
[listen]
resp = 127.0.0.1:17814
http = 127.0.0.1:17815
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PY_EOF'
import json, socket, sys, time, urllib.request

npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17814), 8); s.settimeout(30)
f = s.makefile("rwb")
def cmd(*a):
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    f.flush()
    line = f.readline()
    if line[:1] == b"$":
        n = int(line[1:])
        if n >= 0: f.read(n + 2)
    return line

def rows():
    d = json.load(urllib.request.urlopen("http://127.0.0.1:17815/stats", timeout=10))
    return {c.get("verb") or c.get("name"): c for c in d.get("commands", [])}

# EXPIRE is called exactly once, at the start, and never again: after the
# roll its calls sit before the oldest snapshot, so the window holds none
# of them.  SET keeps going, so the run has a control that must stay live.
cmd("SET", "k", "v")
cmd("EXPIRE", "k", "600")
r = rows()
e = r.get("expire", {})
ok_now = e.get("calls") == 1 and e.get("win_calls") == 1 and e.get("win_max_us", 0) > 0
(ok if ok_now else bad)("before the roll the call is in the window (calls=%s win_calls=%s win_max=%s)"
                        % (e.get("calls"), e.get("win_calls"), e.get("win_max_us")))

# one roll, plus a margin for the 1 Hz tick that drives it
print("  ..   waiting 66 s for the window to roll")
t0 = time.time()
while time.time() - t0 < 66:
    time.sleep(3)
    cmd("SET", "k", "v")          # the control keeps its window alive

r = rows()
e = r.get("expire", {})
st = r.get("set", {})
(ok if e.get("win_calls") == 0 else bad)(
    "after the roll the window holds no expire call (win_calls=%s)" % e.get("win_calls"))
(ok if e.get("calls", 0) >= 1 else bad)(
    "and the lifetime count keeps it (calls=%s)" % e.get("calls"))
(ok if e.get("max_us", 0) > 0 else bad)(
    "and the lifetime max keeps it (max_us=%s)" % e.get("max_us"))
(ok if e.get("win_max_us") == 0 else bad)(
    "the window's MAX agrees with the window's CALLS: no calls, no slowest call "
    "(win_calls=%s win_max_us=%s)" % (e.get("win_calls"), e.get("win_max_us")))
(ok if st.get("win_calls", 0) > 0 and st.get("win_max_us", 0) > 0 else bad)(
    "the control verb still reports both (set win_calls=%s win_max_us=%s)"
    % (st.get("win_calls"), st.get("win_max_us")))
(ok if (e.get("win_p99_us") or 0) == 0 else bad)(
    "and its percentiles read empty too (win_p99_us=%s)" % e.get("win_p99_us"))

print("winmaxtest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
rc=$?
echo "winmaxtest: done (rc=$rc)"
exit $rc
