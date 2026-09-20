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

# S185: the commands table is folded to its slowest verb until asked.
# A browser is needed to see it fold; what can be pinned here is that
# the page ships the parts - the fold class on the wrapper, the hidden
# class on every row after the first, and a handler on the label.
case "$PAGE" in
	*"cmdfold"*"tr.cx{display:none}"*) ok "the commands table ships folded (cmdfold + hidden rows)";;
	*) bad "the commands table has no fold";;
esac
case "$PAGE" in
	*"cmdtog"*"addEventListener('click',cmdfold)"*) ok "and its label opens it";;
	*) bad "the fold has no click handler";;
esac
# S183: the slowest-call column is the exact max when the daemon sends
# one, not a bucket bound derived from the histogram
# S183/S194: the exact max, read through the window accessor
case "$PAGE" in
	*"function wmax(x){var mx=wq(x,'max_us');if(mx>0)return lat(mx)"*)
		ok "the slowest call is the exact max, not a bucket bound";;
	*) bad "the slowest call still comes from the histogram bound";;
esac
# and every column on that table is the last five minutes, not since start
case "$PAGE" in
	*"function wq(x,k){return x['win_'+k]!==undefined?x['win_'+k]:x[k];}"*)
		ok "the commands table reads the five-minute window";;
	*) bad "the commands table is still cumulative";;
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

# S183: every command row carries an exact max beside the percentiles
st = json.loads(urllib.request.urlopen(
    "http://127.0.0.1:17705/stats", timeout=10).read())
cmds = [x for x in (st.get("commands") or []) if x.get("calls")]
(ok if cmds and all("max_us" in x for x in cmds) else bad)(
    "every command row reports an exact max_us (%d rows)" % len(cmds))
# S189: the dialect is a FIELD, not a prefix a reader has to parse.  A
# Redis command name may contain a dot (JSON.GET, TS.ADD), so "json.get"
# could not say whether it was the native JSON door or a RESP client
# sending JSON.GET.
(ok if all(x.get("dialect") and x.get("verb") for x in cmds) else bad)(
    "every row carries its dialect and verb apart from the key (%s)"
    % [(x["name"], x.get("dialect"), x.get("verb")) for x in cmds[:3]])
resp_rows = [x for x in cmds if x["dialect"] == "resp"]
(ok if resp_rows and all(":" not in x["name"] for x in resp_rows) else bad)(
    "a RESP command keeps its bare Redis name and reads as dialect resp")
(ok if cmds and all(x["p50_us"] <= x["p99_us"] <= x["max_us"] or x["calls"] < 2
                    for x in cmds) else bad)(
    "and no percentile exceeds it: %s"
    % [(x["name"], x["p50_us"], x["p99_us"], x["max_us"]) for x in cmds[:3]])

print("loadfactortest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PY_EOF
rc=$?
echo "loadfactortest (page): $pass passed, $fail failed"
[ $rc = 0 ] && [ $fail = 0 ]
