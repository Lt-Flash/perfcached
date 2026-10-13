#!/bin/sh
# statsschedtest.sh - S358: the scheduled stats reset.  /stats and the page
# start counting again every stats_reset days at stats_reset_at, local
# time (default: daily at midnight), so no count they show reaches the
# billions; /metrics is never reset.  PERFCACHED_STATS_RESET beats the
# file - `make check` sets it to off so no suite goes red across midnight.
#   A  stats_reset = 1d at a time 5 s ahead: next_reset is that moment;
#      it resets then (reset_at = that moment, hits 0, /metrics untouched)
#      and next_reset moves to the same time tomorrow
#   B  the same file under PERFCACHED_STATS_RESET=off: no schedule
#      (next_reset 0), and nothing is reset when the moment passes
#   C  stats_reset = 7d: the next reset is a Monday, 00:00 local
#   D  stats_reset = 8d, stats_reset_at = 24:00 and
#      PERFCACHED_STATS_RESET=9d are refused by the config check
# Fail-first: 0.5.6.3 knows neither key and refuses the config.
# Usage: test/statsschedtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcstatssched.XXXXXX); PA= PB= PC=
trap 'for p in $PA $PB $PC; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
mk() { # mk <file> <tcp> <resp> <http> <extra [daemon] lines>
	printf '[daemon]\nworkers = 1\n%s\n[memory]\narena_mb = 32\n[secrets]\nclient = ss-client-secret\n[listen]\ntcp = 127.0.0.1:%s\nresp = 127.0.0.1:%s\nhttp = 127.0.0.1:%s\nplaintext = loopback\n[collection 0]\nbuckets_log2 = 8\n' "$5" "$2" "$3" "$4" > "$1"
	chmod 640 "$1"
}
up() { # up <conf> <log> - start, wait ready; echoes the pid
	"$BIN" -f "$1" > "$2" 2>&1 & p=$!
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$2" && break; kill -0 $p 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	echo $p
}
set -- $(date -d '+5 sec' '+%s %H:%M:%S'); AT=$1; HMS=$2
mk "$D/a.conf" 18934 18935 18936 "stats_reset = 1d
stats_reset_at = $HMS"
mk "$D/b.conf" 18937 18938 18939 "stats_reset = 1d
stats_reset_at = $HMS"
PA=$(unset PERFCACHED_STATS_RESET; up "$D/a.conf" "$D/a.log")
PB=$(export PERFCACHED_STATS_RESET=off; up "$D/b.conf" "$D/b.log")
grep -q "perfcached ready" "$D/a.log" && grep -q "perfcached ready" "$D/b.log" \
	|| { bad "the daemons did not start: $(grep -h -m1 -E 'ERR|CRIT' "$D/a.log" "$D/b.log" | head -1 | cut -c1-140)"; echo "statsschedtest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" AT=$AT timeout 60 python3 - <<'PY' 2> "$D/py.err"
import json, os, re, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
AT = int(os.environ["AT"])
def stats(h): return json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % h, timeout=5).read())
def metric_hits(h):
    mt = urllib.request.urlopen("http://127.0.0.1:%d/metrics" % h, timeout=5).read().decode()
    m = re.search(r'perfcached_collection_hits_total\{collection="0"\} (\d+)', mt)
    return int(m.group(1)) if m else None
def hits(st): return [c for c in st["collections"] if c.get("name") == "0"][0].get("hits")
def traffic(port):
    s = socket.create_connection(("127.0.0.1", port), 5); f = s.makefile("rb")
    def cmd(*a):
        s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a)); h = f.readline()
        if h[:1] == b"$" and int(h[1:]) >= 0: f.read(int(h[1:]) + 2)
    cmd(b"SET", b"k", b"v")
    for _ in range(3): cmd(b"GET", b"k")
    s.close()
lt = time.localtime(AT)
nxt = int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday + 1, lt.tm_hour, lt.tm_min, lt.tm_sec, 0, 0, -1)))
a0, b0 = stats(18936), stats(18939)
check(a0["since"].get("next_reset") == AT, "A: next_reset is the scheduled moment (%s, want %s)" % (a0["since"].get("next_reset"), AT))
check(b0["since"].get("next_reset") == 0, "B: PERFCACHED_STATS_RESET=off leaves no schedule (next_reset %s)" % b0["since"].get("next_reset"))
traffic(18935); traffic(18938); time.sleep(0.3)
ha, hb, ma = hits(stats(18936)), hits(stats(18939)), metric_hits(18936)
check(ha == 3 and hb == 3 and ma == 3, "before the moment: 3 hits on each node, /metrics 3 (A %s, B %s, A /metrics %s)" % (ha, hb, ma))
while time.time() < AT + 2.5:
    time.sleep(0.25)
a1, b1 = stats(18936), stats(18939)
check(a1["since"].get("reset_at") == AT and hits(a1) == 0,
      "A: reset at the scheduled moment (reset_at %s, want %s; hits %s)" % (a1["since"].get("reset_at"), AT, hits(a1)))
check(a1["since"].get("next_reset") == nxt, "A: the next reset is the same time tomorrow (%s, want %s)" % (a1["since"].get("next_reset"), nxt))
check(metric_hits(18936) == 3, "A: /metrics was not reset (%s)" % metric_hits(18936))
check(b1["since"].get("reset_at") == 0 and hits(b1) == 3,
      "B: nothing reset when the moment passed (reset_at %s, hits %s)" % (b1["since"].get("reset_at"), hits(b1)))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
grep -q "statistics reset: the 1-day schedule" "$D/a.log" && ok "A: the reset is logged" || bad "A: no 'statistics reset' line in the log"
kill $PA $PB 2>/dev/null; wait $PA $PB 2>/dev/null; PA= PB=

# C: weekly lands on a Monday at 00:00
mk "$D/c.conf" 18934 18935 18936 "stats_reset = 7d"
PC=$(unset PERFCACHED_STATS_RESET; up "$D/c.conf" "$D/c.log")
if grep -q "perfcached ready" "$D/c.log"; then
	r=$(curl -s -m 5 http://127.0.0.1:18936/stats | python3 -c '
import json,sys,time
n=json.load(sys.stdin)["since"].get("next_reset") or 0
t=time.localtime(n) if n else None
ok = bool(t) and t.tm_wday == 0 and (t.tm_hour, t.tm_min, t.tm_sec) == (0, 0, 0) and time.time() < n <= time.time() + 7 * 86400 + 3600
print("ok" if ok else "bad", time.strftime("%a %Y-%m-%d %H:%M:%S", t) if t else n)')
	case "$r" in ok*) ok "C: 7d - the next reset is a Monday at 00:00 (${r#ok })" ;; *) bad "C: 7d - next reset is not a Monday 00:00 (${r#bad })" ;; esac
else
	bad "C: the daemon did not start with stats_reset = 7d"
fi
kill $PC 2>/dev/null; wait $PC 2>/dev/null; PC=

# D: what is refused
mk "$D/d1.conf" 18934 18935 18936 "stats_reset = 8d"
mk "$D/d2.conf" 18934 18935 18936 "stats_reset_at = 24:00"
mk "$D/d3.conf" 18934 18935 18936 ""
env -u PERFCACHED_STATS_RESET "$BIN" -C -f "$D/d1.conf" > "$D/d1.out" 2>&1 && bad "D: stats_reset = 8d was accepted" || ok "D: stats_reset = 8d is refused"
env -u PERFCACHED_STATS_RESET "$BIN" -C -f "$D/d2.conf" > "$D/d2.out" 2>&1 && bad "D: stats_reset_at = 24:00 was accepted" || ok "D: stats_reset_at = 24:00 is refused"
PERFCACHED_STATS_RESET=9d "$BIN" -C -f "$D/d3.conf" > "$D/d3.out" 2>&1 && bad "D: PERFCACHED_STATS_RESET=9d was accepted" || ok "D: PERFCACHED_STATS_RESET=9d is refused"
env -u PERFCACHED_STATS_RESET "$BIN" -C -f "$D/d3.conf" > "$D/d3b.out" 2>&1 && ok "D: the defaults pass the check (positive control)" || bad "D: the default config was refused: $(tail -1 "$D/d3b.out")"
echo "statsschedtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
