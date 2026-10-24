#!/bin/sh
# sketchtest.sh - 12nx (0.7.1): the fleet's distinct keys in store mode.
# Each member's held walk builds a HyperLogLog sketch of the keys it holds
# (the `sketch` verb: 4,096 one-byte registers, one fixed hash on every
# node); the dashboard merges the members' sketches - the max of each
# register is the sketch of the union - and /fleet reports the estimate.
# Two store-mode members: keys 0-1999 written through A, 1000-2999 through
# B, so each holds 2,000 and the fleet 3,000 DISTINCT keys (1,000 on both).
# The hash and the keys are fixed, so the estimate is the same on every run
# and the bounds below are tight on purpose: p = 12's standard error is
# 1.6%, these allow 4%.
#   A. the verb: p 12, 4,096 registers, a walk time
#   B. each member alone: about 2,000
#   C. /fleet: the union about 3,000 - not 4,000 (the shared keys counted
#      once) and not 2,000 (the fullest member, store mode's lower bound)
#   D. an older daemon or a collection with no walk yet: estimate null, not
#      a wrong number (a collection created after the start)
# Fail-first: before 0.7.1 there is no sketch verb and /fleet has no
# "distinct".
# Usage: test/sketchtest.sh [./perfcached] [./perfcached-dashboard]
set -u
BIN=${1:-./perfcached}
DASH=${2:-./perfcached-dashboard}
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
[ -x "$DASH" ] || { echo "sketchtest: SKIPPED - $DASH not built (libmicrohttpd-dev absent?)"; exit 0; }
D=$(mktemp -d /var/tmp/pcsketch.XXXXXX)
TCP1=16971 TCP2=16972 HP=16975 MC=16977
PA= PB= PD=
trap 'for p in $PA $PB $PD; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $TCP1 $TCP2 $HP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "sketchtest: port $p already bound" >&2; exit 1; }
done

node() { # node <n> <tcp>
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = sk-client-secret
cluster = sk-cluster-secret
[listen]
tcp = 127.0.0.7$1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.97:$MC
advertise = 127.0.0.7$1
mode = store
[collection sk]
buckets_log2 = 12
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=$!"
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
node 1 $TCP1; node 2 $TCP2
start 1 A; start 2 B
sleep 3.5

RES="$D/res"; : > "$RES"
RESFILE="$RES" TCP1=$TCP1 TCP2=$TCP2 python3 - <<'PY'
import base64, json, os, socket, time
import pcnative
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def conn(n, port):
    return pcnative.wrap(socket.create_connection(("127.0.0.7%d" % n, port), 10))
A = conn(1, int(os.environ["TCP1"])); B = conn(2, int(os.environ["TCP2"]))
for i in range(0, 2000):
    pcnative.call(A, "set", col="sk", key="key:%d" % i, value="v")
for i in range(1000, 3000):
    pcnative.call(B, "set", col="sk", key="key:%d" % i, value="v")
t0 = int(time.time())
def sketch(f):
    r = pcnative.call(f, "sketch", col="sk")
    return r.get("result"), r.get("error")
# the walks run every 5 s on a small table; wait for one that began after the writes
deadline = time.time() + 40
while time.time() < deadline:
    a, _ = sketch(A); b, _ = sketch(B)
    if a and b and a["walked_at"] > t0 and b["walked_at"] > t0:
        break
    time.sleep(1)
a, ea = sketch(A)
regs = base64.b64decode(a["registers"]) if a else b""
check(a and a["p"] == 12 and len(regs) == 4096 and a["walked_at"] > t0 and a["hash"] == "fnv1a64+fmix64",
      "A. the sketch verb: p 12, 4,096 registers, a walk after the writes (%r)" % (ea or {k: a[k] for k in ("p", "walked_at", "hash")}))
import math
def est(r):
    m = 4096.0; alpha = 0.7213 / (1 + 1.079 / m)
    s = sum(2.0 ** -x for x in r); z = r.count(0)
    e = alpha * m * m / s
    return m * math.log(m / z) if e <= 2.5 * m and z else e
b, _ = sketch(B)
ea_, eb_ = est(list(regs)), est(list(base64.b64decode(b["registers"])))
check(1920 <= ea_ <= 2080 and 1920 <= eb_ <= 2080, "B. each member alone about 2,000: %.0f and %.0f" % (ea_, eb_))
with open(os.environ["RESFILE"] + ".est", "w") as f:
    f.write("%.0f %.0f\n" % (ea_, eb_))
PY
cat > "$D/dash.conf" <<C
[dashboard]
listen = 127.0.0.1:$HP
seed = 127.0.0.71:$TCP1
secret =
cache_ms = 300
refresh_ms = 500
C
chmod 600 "$D/dash.conf"
"$DASH" -f "$D/dash.conf" > "$D/dash.out" 2> "$D/dash.log" & PD=$!
i=0; while [ $i -lt 50 ] && ! grep -q "perfcached-dashboard ready" "$D/dash.out"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached-dashboard ready" "$D/dash.out" || { bad "the dashboard did not start: $(tail -3 "$D/dash.log")"; }
sleep 1.5                                  # it learns the second member from the first
curl -s "http://127.0.0.1:$HP/fleet" > "$D/fleet.json"
F=$(python3 -c '
import json; d = json.load(open("'"$D"'/fleet.json"))
s = d.get("distinct", {}).get("sk") or {}
print(s.get("estimate"), s.get("members"), s.get("of"), sorted(c["entries"] for m in d["members"] for c in m["collections"] if c["name"] == "sk"))' 2>&1)
set -- $F
[ "$#" -ge 3 ] && [ "$1" != None ] && [ "$1" -ge 2880 ] 2>/dev/null && [ "$1" -le 3120 ] && [ "$2" = 2 ] && [ "$3" = 2 ] &&
	ok "C. /fleet: the union of the two sketches about 3,000 ($1) - the 1,000 shared keys counted once, from 2 of 2 members" ||
	bad "C. /fleet distinct: $F"
echo "$F" | grep -q '\[2000, 2000\]' && ok "C. and each member holds 2,000 (the fullest member, store's bound, would say 2,000)" || bad "C. member entries: $F"
# D: a collection created now has no walk yet - null, not a number
python3 - "$TCP1" <<'PY' > /dev/null 2>&1
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.71", int(sys.argv[1])), 10))
pcnative.call(f, "create", col="fresh", buckets_log2=10)
PY
sleep 1
curl -s "http://127.0.0.1:$HP/fleet" > "$D/fleet2.json"
G=$(python3 -c '
import json; d = json.load(open("'"$D"'/fleet2.json")); s = d.get("distinct", {}).get("fresh")
print("absent" if s is None else s.get("estimate"))' 2>&1)
case "$G" in None|absent|[0-9]*) ok "D. a collection with no walk yet: '$G' (null or a fresh walk's figure, never an error)";; *) bad "D. fresh: $G";; esac
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "the driver produced no result"
for p in $PD $PA $PB; do kill $p 2>/dev/null; done; wait 2>/dev/null; PA= PB= PD=
echo "sketchtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
