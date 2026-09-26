#!/bin/sh
# cmdstatstest.sh - S159: the native doors count into the same command
# rows and the same slow log as the RESP door, under their dialect's
# name (json:get, bin:get - S189 uses a COLON, since a Redis command
# name may contain a dot; RESP keeps its bare Redis name), and the
# rows are reachable off the RESP door: /stats carries `commands` and
# `slowlog`, /metrics the per-command counters and a log2 latency
# histogram, INFO latencystats the p50/p99/p99.9 bounds in Redis 7's
# shape.  Counts are asserted EXACTLY: the drivers are raw sockets, so
# nothing injects hidden commands.
#
# Daemon A logs every command (slowlog_usec = 0); daemon B's threshold
# is ten seconds, so it must count and never log.
# Fail-first: 0.4.0-rc1 has no json:* row, no `commands` on /stats and
# no latencystats section.
# Usage: test/cmdstatstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccs.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkconf() { # mkconf <file> <tcp> <resp> <http> <slowlog_usec-line>
	cat > "$1" <<CONF
[daemon]
workers = 2
log_level = notice
$5
[memory]
arena_mb = 64
[secrets]
client = cs-client-secret
cluster = cs-cluster-secret
[listen]
tcp = 127.0.0.1:$2
resp = 127.0.0.1:$3
http = 127.0.0.1:$4
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
	chmod 600 "$1"
}
start() { # start <conf> <log> -> pid in $STARTED
	"$BIN" -f "$1" > "$2" 2>&1 &
	STARTED=$!
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$2" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; cat "$2"; return 1
}

mkconf "$D/a.conf" 17761 17762 18761 "slowlog_usec = 0"
start "$D/a.conf" "$D/a.log" || exit 1
P1=$STARTED
mkconf "$D/b.conf" 17763 17764 18763 "slowlog_usec = 10000000"
start "$D/b.conf" "$D/b.log" || exit 1
P2=$STARTED

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, re, socket, struct, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n")

MAGIC = 0x9E
def freq(payload, rid):
    return struct.pack("<BBBBIQ", MAGIC, 1, 1, 0, len(payload), rid) + payload
def vh(verb, col, key, extra=b"", val=b""):
    return bytes([verb, len(col)]) + struct.pack("<H", len(key)) + \
        extra + col + key + val
def rexact(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: raise EOFError
        b += d
    return b
def bmsg(s):
    h = rexact(s, 16)
    magic, ver, typ, flags, ln, rid = struct.unpack("<BBBBIQ", h)
    return rid, flags, rexact(s, ln)

def json_traffic(port, nget, nset):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    f = s.makefile("rwb"); rid = 0
    def req(method, params):
        nonlocal rid
        rid += 1
        f.write((json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                             "params": params}) + "\n").encode()); f.flush()
        return json.loads(f.readline())
    for i in range(nset): req("set", {"col": "0", "key": "k%d" % i, "value": "v"})
    for i in range(nget): req("get", {"col": "0", "key": "k"})
    f.close(); s.close()

def bin_traffic(port, nget, nset):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    rid = 0
    for i in range(nset):
        rid += 1
        s.sendall(freq(vh(3, b"0", b"k%d" % i, struct.pack("<q", 0), b"v"), rid)); bmsg(s)
    for i in range(nget):
        rid += 1
        s.sendall(freq(vh(2, b"0", b"k"), rid)); bmsg(s)
    s.close()

class Resp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rwb")
    def _one(self):
        line = self.f.readline()
        t, body = line[:1], line[1:].strip()
        if t == b"$":
            n = int(body)
            if n < 0: return None
            d = self.f.read(n + 2); return d[:n].decode("utf-8", "replace")
        if t == b"*":
            n = int(body)
            return None if n < 0 else [self._one() for _ in range(n)]
        if t == b":": return int(body)
        return (t + body).decode()
    def cmd(self, *a):
        self.f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
        self.f.flush()
        return self._one()

def stats(port):
    return json.load(urllib.request.urlopen("http://127.0.0.1:%d/stats" % port, timeout=5))
def metrics(port):
    return urllib.request.urlopen("http://127.0.0.1:%d/metrics" % port, timeout=5).read().decode()

# ---- daemon A: every command lands in the slow log ----------------------
json_traffic(17761, 40, 3)
bin_traffic(17761, 40, 2)
r = Resp(17762)
for i in range(10): r.cmd("GET", "k")
cst = r.cmd("INFO", "commandstats") or ""
def cs(name):
    m = re.search(r"cmdstat_%s:calls=(\d+),usec=(\d+),usec_per_call=([0-9.]+)" % re.escape(name), cst)
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)
check(cs("json:get") == (40, cs("json:get")[1]) and (cs("json:get")[1] or 0) > 0,
      "cmdstat_json:get: exactly 40 calls, usec>0, the plugin's shape (%r)" % (cs("json:get"),))
check(cs("json:set")[0] == 3, "cmdstat_json:set: exactly 3 calls (%r)" % (cs("json:set")[0],))
check(cs("bin:get")[0] == 40, "cmdstat_bin:get: exactly 40 calls (%r)" % (cs("bin:get")[0],))
check(cs("bin:set")[0] == 2, "cmdstat_bin:set: exactly 2 calls (%r)" % (cs("bin:set")[0],))
check(cs("get")[0] == 10, "cmdstat_get: the RESP row keeps its bare name and its own count, 10 (%r)" % (cs("get")[0],))

lst = r.cmd("INFO", "latencystats") or ""
m = re.search(r"latencystat_json:get:p50=([0-9.]+),p99=([0-9.]+),p99\.9=([0-9.]+)", lst)
if m:
    p50, p99, p999 = (float(x) for x in m.groups())
    check(0 < p50 <= p99 <= p999, "latencystat_json:get: p50<=p99<=p99.9 in ms, Redis 7's shape (%s)" % m.group(0))
else:
    check(False, "latencystat_json:get missing from INFO latencystats: %r" % lst[:80])
check(re.search(r"latencystat_bin:get:p50=", lst) is not None and re.search(r"latencystat_get:p50=", lst) is not None,
      "latencystats carries bin:get and the bare RESP get")
bare = r.cmd("INFO") or ""
check("latencystat_" not in bare and "cmdstat_" not in bare, "bare INFO excludes latencystats as it excludes commandstats")
alls = r.cmd("INFO", "all") or ""
check("# Latencystats" in alls and "latencystat_json:get:" in alls, "INFO all carries the Latencystats section")

st = stats(18761)              # before SLOWLOG GET, which logs itself
sl = r.cmd("SLOWLOG", "GET", "200") or []
argvs = [e[3] for e in sl if isinstance(e, list) and len(e) == 6]
check(["json:get", "0", "k"] in argvs, "a JSON get sits in the slow log as [json:get, col, key]")
check(["bin:get", "0", "k"] in argvs, "a binary get sits in the slow log as [bin:get, col, key]")
check(["bin:set", "0", "k1"] in argvs, "a binary set logs verb, collection, key - not the value")
check(sum(1 for a in argvs if a[:1] == ["json:get"]) == 40, "all 40 JSON gets logged (%d)" % sum(1 for a in argvs if a[:1] == ["json:get"]))

cmds = {c.get("name"): c for c in st.get("commands", [])}
jg = cmds.get("json:get", {})
check(jg.get("calls") == 40 and len(jg.get("hist", [])) == 129 and sum(jg.get("hist", [])) == 40,
      "/stats commands: json:get calls=40 with 129 buckets summing to the calls (%r)" % (jg.get("calls"),))
check(cmds.get("bin:get", {}).get("calls") == 40 and cmds.get("get", {}).get("calls") == 10,
      "/stats commands: bin:get 40, get 10")
check(all(sum(c.get("hist", [])) == c.get("calls") for c in cmds.values()) and len(cmds) >= 6,
      "/stats commands: bucket sums equal calls on every row (%d rows)" % len(cmds))
check(1 <= jg.get("p50_us", 0) <= jg.get("p99_us", 0) <= jg.get("p999_us", 0),
      "/stats commands: p50_us<=p99_us<=p999_us, all >=1 (%s/%s/%s)" % (jg.get("p50_us"), jg.get("p99_us"), jg.get("p999_us")))
# S183: eight buckets per octave and interpolation inside the bucket, so
# a percentile estimates the VALUE.  It must never exceed the exact max,
# which is the thing the histogram cannot overstate.
check(jg.get("max_us", 0) > 0 and jg.get("p999_us", 0) <= jg.get("max_us", 0),
      "/stats commands: an exact max_us, and no percentile above it (%s <= %s)"
      % (jg.get("p999_us"), jg.get("max_us")))
ids_resp = [e[0] for e in sl if isinstance(e, list) and len(e) == 6][:32]
ids_stat = [e.get("id") for e in st.get("slowlog", [])]
check(ids_stat == ids_resp and len(ids_stat) == 32, "/stats slowlog mirrors SLOWLOG GET: the newest 32, same ids, same order")
e0 = (st.get("slowlog") or [{}])[0]
check(set(e0.keys()) == {"id", "ts", "usec", "argv", "addr", "name"}, "/stats slowlog entries carry the six SLOWLOG fields")
jent = [e for e in st.get("slowlog", []) if e.get("argv", [])[:1] == ["bin:get"]]
check(bool(jent) and jent[0]["argv"] == ["bin:get", "0", "k"] and jent[0]["addr"].startswith("127.0.0.1:"),
      "/stats slowlog: a native entry with its argv and the client address")

mt = metrics(18761)
def mval(pat):
    m = re.search(pat + r" ([0-9.]+)\n", mt)
    return float(m.group(1)) if m else None
check(mval(r'perfcached_command_calls_total\{cmd="json:get"\}') == 40 and mval(r'perfcached_command_calls_total\{cmd="bin:get"\}') == 40
      and mval(r'perfcached_command_calls_total\{cmd="get"\}') == 10, "/metrics command_calls_total per name: json:get 40, bin:get 40, get 10")
check((mval(r'perfcached_command_usec_total\{cmd="json:get"\}') or 0) == jg.get("usec") and jg.get("usec", 0) > 0,
      "/metrics command_usec_total{json:get} equals the /stats usec")
bk = re.findall(r'perfcached_command_latency_seconds_bucket\{cmd="json:get",le="([^"]+)"\} (\d+)\n', mt)
les = [b[0] for b in bk]; cums = [int(b[1]) for b in bk]
# S183: 128 bounds - eight per octave - and +Inf.  The first three are
# the exact-microsecond slots below PC_OBS_SUB, so le=1e-6 is still the
# first and the bounds still rise monotonically.
# le=0 is the sub-microsecond bucket: below PC_OBS_SUB every value has
# its own slot, and the first of them is "rounded to 0 us".
check(len(bk) == 129 and les[0] == "0.000000" and les[1] == "0.000001" and les[128] == "+Inf"
      and [float(x) for x in les[:128]] == sorted(float(x) for x in les[:128]),
      "/metrics histogram: 128 rising bounds and +Inf (%d rows, first %s)"
      % (len(bk), les[0] if les else "none"))
check(cums == sorted(cums) and cums and cums[-1] == 40 and mval(r'perfcached_command_latency_seconds_count\{cmd="json:get"\}') == 40,
      "/metrics histogram: cumulative, +Inf = count = 40")
check(abs((mval(r'perfcached_command_latency_seconds_sum\{cmd="json:get"\}') or -1) - jg.get("usec", 0) / 1e6) < 1e-6,
      "/metrics histogram: _sum is the usec total in seconds")
check("# TYPE perfcached_command_latency_seconds histogram" in mt, "/metrics histogram declared as a histogram")

# ---- daemon B: a ten-second threshold counts and never logs -------------
json_traffic(17763, 5, 0)
bin_traffic(17763, 4, 0)
rb = Resp(17764)
cstb = rb.cmd("INFO", "commandstats") or ""
check(re.search(r"cmdstat_json:get:calls=5,", cstb) is not None and re.search(r"cmdstat_bin:get:calls=4,", cstb) is not None,
      "threshold 10 s: json:get 5 and bin:get 4 counted")
check(rb.cmd("SLOWLOG", "LEN") == 0, "threshold 10 s: SLOWLOG LEN is 0")
stb = stats(18763)
check(stb.get("slowlog") == [] and {c.get("name"): c.get("calls") for c in stb.get("commands", [])}.get("json:get") == 5,
      "threshold 10 s: /stats slowlog empty, commands still counted")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "cmdstatstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
