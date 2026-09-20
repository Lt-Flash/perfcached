#!/bin/sh
# loadfactortest.sh - S180: the LOAD column is a load FACTOR.
#
# The status page divided entries by buckets and ignored the six slots
# in each, so a table at half its capacity reported "300.00%".  The
# operator read that column, on a fleet holding 63,968 records in 21,323
# buckets - 50% - and asked for a warning threshold on it.  A number six
# times too large is worse than no number.
#
# What can be asserted without a browser: the daemon publishes the slots
# per bucket, its own arithmetic agrees with entries/(buckets x slots),
# and the page's script uses that form and carries the thresholds.
# Usage: test/loadfactortest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pclf.XXXXXX)
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
client = lf-client-secret
[listen]
resp = 127.0.0.1:17704
http = 127.0.0.1:17705
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/a.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

PAGE=$(python3 -c "
import urllib.request
print(urllib.request.urlopen('http://127.0.0.1:17705/', timeout=10).read().decode('utf-8','replace'))
")

# the page must divide by buckets x slots, and must carry both thresholds
case "$PAGE" in
	*"x.buckets*(x.slots||6)"*) ok "the page computes load against buckets x SLOTS";;
	*) bad "the page still divides entries by buckets alone";;
esac
case "$PAGE" in
	*"ld>=90"*"ld>=70"*) ok "and carries the operator's thresholds (amber 70, red 90)";;
	*) bad "the load column has no 70/90 thresholds";;
esac

python3 - <<'PY_EOF'
import json, socket, sys, urllib.request

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

s = socket.create_connection(("127.0.0.1", 17704), 8); s.settimeout(30)
f = s.makefile("rwb")
def cmd(*a):
    o = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        o += b"$%d\r\n%s\r\n" % (len(x), x)
    return o
def col():
    st = json.loads(urllib.request.urlopen(
        "http://127.0.0.1:17705/stats", timeout=10).read())
    return [c for c in st["collections"] if c["name"] == "0"][0]

# 12,288 records into 4,096 buckets: half of 4,096 x 6
N = 12288
for base in range(0, N, 1024):
    s.sendall(b"".join(cmd("SET", b"lf:%d" % (base+j), b"v") for j in range(1024)))
    f.flush()
    for _ in range(1024):
        f.readline()

c = col()
(ok if c.get("slots") else bad)("the collection publishes its slots per bucket (%s)" % c.get("slots"))
slots = c.get("slots") or 0
ld = 100.0 * c["entries"] / (c["buckets"] * slots) if slots and c["buckets"] else -1
(ok if 45 <= ld <= 55 else bad)(
    "%d records in %d buckets x %s slots is %.1f%% - a load FACTOR, not %.0f%%"
    % (c["entries"], c["buckets"], slots, ld,
       100.0 * c["entries"] / c["buckets"]))

print("loadfactortest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PY_EOF
rc=$?
echo "loadfactortest (page): $pass passed, $fail failed"
[ $rc = 0 ] && [ $fail = 0 ]
