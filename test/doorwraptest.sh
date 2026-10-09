#!/bin/sh
# doorwraptest.sh - S360: a door total that wraps its 32 bits keeps
# counting up.  The per-door connection and request totals are 32-bit
# relaxed atomics (no cross-thread 64-bit atomics), and at a few thousand
# requests a second one passes 4,294,967,295 in days; /stats then showed
# its since-reset figure as cur - base: negative, printed as ~1.8e19.
# They are widened to 64 bits on the read side now.
#   - PERFCACHED_TEST_DOOR_START starts every door counter 1,000 below the
#     wrap (the daemon logs that it did)
#   - 500 RESP requests, a reset (the page's), 1,000 more: the RESP door's
#     requests cross the wrap and read 1,000 since the reset
#   - 1,500 requests on the native door (RESP dialect) cross it from the
#     start: they read 1,500
#   - every figure stays put across the maintenance thread's fold, and no
#     total or 5-minute window reads negative or past 2^32
# Fail-first: 0.5.6.3 has no PERFCACHED_TEST_DOOR_START.  The bug itself,
# shown on 0.5.6.3 with gdb setting pc_resp_reqs 1,000 below the wrap:
# 1,500 requests after a reset read -4294965796.
# Usage: test/doorwraptest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcdoorwrap.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 32
[secrets]
client = dw-client-secret
[listen]
tcp = 127.0.0.1:19491
resp = 127.0.0.1:19492
http = 127.0.0.1:19493
plaintext = loopback
[collection 0]
buckets_log2 = 8
CONF
chmod 640 "$D/n.conf"
PERFCACHED_TEST_DOOR_START=4294966296 "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; kill -0 $P1 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(grep -m1 -E 'ERR|CRIT' "$D/n.log" | cut -c1-140)"; echo "doorwraptest: $pass passed, $fail failed"; exit 1; }
grep -q "test: the door counters start at 4294966296" "$D/n.log" \
	&& ok "the door counters start 1,000 below the wrap (PERFCACHED_TEST_DOOR_START)" \
	|| bad "PERFCACHED_TEST_DOOR_START was not honoured - no counter is near its wrap"

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 60 python3 - <<'PY' 2> "$D/py.err"
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def stats(): return json.loads(urllib.request.urlopen("http://127.0.0.1:19493/stats", timeout=5).read())
def pings(port, n):
    s = socket.create_connection(("127.0.0.1", port), 5); f = s.makefile("rb")
    for _ in range(n):
        s.sendall(b"*1\r\n$4\r\nPING\r\n"); f.readline()
    s.close()
def sane(v): return isinstance(v, int) and 0 <= v < 2 ** 32
pings(19492, 500)
time.sleep(0.3)
st = stats()
check(st["resp"]["requests"] == 500, "500 RESP requests read 500 (%s)" % st["resp"]["requests"])
r = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:19493/reset-stats", data=b"", method="POST"), timeout=5).read())
pings(19492, 1000)                     # crosses 2^32 at the 1,000th since start
pings(19491, 1500)                     # the native door's RESP dialect, from 1,000 below
time.sleep(0.3)
st = stats()
rq, nq = st["resp"]["requests"], st["native"]["resp"]["requests"]
check(rq == 1000, "the RESP door's requests crossed the wrap and read 1000 since the reset (%s)" % rq)
check(nq == 1500, "the native door's RESP requests crossed it and read 1500 since the reset (%s)" % nq)
check(sane(st["resp"]["conns"]) and sane(st["native"]["resp"]["conns"]),
      "connection totals are sane (%s, %s)" % (st["resp"]["conns"], st["native"]["resp"]["conns"]))
w = [st["resp"].get("win_requests"), st["native"]["resp"].get("win_requests")]
check(all(v is None or sane(v) for v in w), "5-minute windows never read negative or past 2^32 (%s)" % w)
time.sleep(2.2)                        # two maintenance folds
st2 = stats()
check(st2["resp"]["requests"] == 1000 and st2["native"]["resp"]["requests"] == 1500,
      "the figures stay put across the maintenance thread's fold (%s, %s)" % (st2["resp"]["requests"], st2["native"]["resp"]["requests"]))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
echo "doorwraptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
