#!/bin/sh
# upttltest.sh - S449: a read-through copy never outlives the upstream's key.
#
# A fetched copy took the key's time left (PTTL, ms) rounded UP to whole
# seconds, on a clock that ticks in whole seconds - so it answered for up to
# a second after the upstream had expired the key.  Staging, 2026-10-25: a
# registration's device_info expired in Redis, EXISTS through perfcached
# still said 1, and the script's JSON.SET $.status went to Redis and was
# refused ("new objects must be created at the root").  Now the copy's
# expiry is the last tick boundary at or before the key's deadline: it goes
# no later than the key, and is held for all but that fraction of a tick.
# One node, collection 0 reading through from test/fakeupstream.py (which
# expires its keys lazily and pushes no invalidation for an expiry - the
# copy lives by its own deadline alone):
#   1. ten keys probed by EXISTS (a present mark) and ten read by GET (a
#      value copy), their TTLs 100 ms apart across a second, so a rounding
#      up overshoots on most whatever the clock's phase: polled every 40 ms
#      past their deadlines, none answers after the upstream's deadline
#   2. the copies did serve - the upstream was asked about any one key in
#      under half the polls (each poll would ask, held nothing; the last
#      fraction of a tick before the deadline does ask, by design)
# FAIL-FIRST: before S449 most keys answer up to a second past the deadline.
# Usage: test/upttltest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=19571 TCP=19572 RESP=19573 HTTP=19574
for p in $UP $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "upttltest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcupttl.XXXXXX)
PID= FPID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; [ -n "$FPID" ] && kill -9 $FPID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$D/up" "$D/state/wal"
python3 - "$D/up/data.json" <<'P'
import json, sys
d = {}
for i in range(10):
    d["tok:e%d" % i] = {"type": "string", "value": "e%d" % i, "ttl_ms": 12000 + 100 * i}
    d["tok:g%d" % i] = {"type": "string", "value": "g%d" % i, "ttl_ms": 12050 + 100 * i}
json.dump(d, open(sys.argv[1], "w"))
P
echo '{"password": "tt-pass-1", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = tt-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
resp_collections = 0
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_password = tt-pass-1
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the node did not start: $(grep -m1 -E 'CRIT|ERR' "$D/n.log" | cut -c1-160)"
	echo "upttltest: $pass passed, $fail failed"
	exit 1
fi
k=0; while [ $k -lt 40 ] && ! python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
raise SystemExit(0 if [c for c in r["collections"] if c["name"]=="0"][0]["upstream"]["tracking"] else 1)' 2>/dev/null; do sleep 0.1; k=$((k+1)); done

R=$(python3 - $RESP $UP <<'P'
import json, socket, sys, time
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x.encode()) for x in a)
class C:
    def __init__(s, port, pw=None):
        s.k = socket.create_connection(("127.0.0.1", port), 5); s.f = s.k.makefile("rb")
        if pw:
            s.cmd("AUTH", pw)
    def cmd(s, *a):
        s.k.sendall(enc(*a)); l = s.f.readline(); t, r = l[:1], l[1:-2]
        if t == b":": return int(r)
        if t == b"$":
            n = int(r)
            return None if n < 0 else s.f.read(n + 2)[:-2].decode()
        if t == b"_": return None
        return (t + r).decode()
pc, up = C(int(sys.argv[1])), C(int(sys.argv[2]), "tt-pass-1")
ek = ["tok:e%d" % i for i in range(10)]; gk = ["tok:g%d" % i for i in range(10)]
dl = {}
for k in ek + gk:
    p = up.cmd("PTTL", k); dl[k] = time.time() + p / 1000.0
first = {k: (pc.cmd("EXISTS", k) if k in ek else pc.cmd("GET", k)) for k in ek + gk}
last = {}
end = max(dl.values()) + 1.5
polls = 0
while time.time() < end:
    for k in ek + gk:
        if time.time() > dl[k] + 1.2:
            continue
        a = pc.cmd("EXISTS", k) if k in ek else pc.cmd("GET", k)
        if (a == 1 if k in ek else a is not None):
            last[k] = time.time()
    polls += 1
    time.sleep(0.04)
over = {k: round((last.get(k, 0) - dl[k]) * 1000) for k in ek + gk}
w = max(over, key=over.get)
print(json.dumps({"first_ok": sum(1 for k in ek if first[k] == 1) + sum(1 for k in gk if first[k] == k[4:]),
                  "worst": w, "over_ms": over[w], "late": sum(1 for v in over.values() if v > 50),
                  "polls": polls}))
P
)
FO=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["first_ok"])' 2>/dev/null)
OV=$(echo "$R" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["over_ms"], d["late"], d["worst"], d["polls"])' 2>/dev/null)
set -- $OV
[ "${FO:-0}" = 20 ] && [ "${2:-99}" = 0 ] &&
	ok "1. 20 copies (10 EXISTS marks, 10 GET values) polled past their deadlines: none answered later than 50 ms after the upstream's (worst: $3, $1 ms; $4 polls)" ||
	bad "1. copies past the upstream's deadline: $2 of 20 answered later than 50 ms after it (worst $3: $1 ms after); first reads right: ${FO:-?} of 20; $R"
MX=0; for i in 0 1 2 3 4 5 6 7 8 9; do
	for c in "TYPE tok:e$i" "GET tok:g$i"; do
		n=$(grep -c "^$c\$" "$D/up/counts" 2>/dev/null); [ "${n:-0}" -gt $MX ] && MX=$n
	done
done
[ "${4:-0}" -ge 30 ] && [ $((MX * 2)) -lt "${4:-0}" ] &&
	ok "2. the copies served: the upstream was asked about a key $MX time(s) at most in ${4:-?} polls" ||
	bad "2. the upstream was asked about one key $MX times in ${4:-?} polls (want under half, and 30 polls or more) - nothing was held, and step 1 proves nothing"

grep -q "CRIT" "$D/n.log" && bad "the node logged CRIT: $(grep -m2 CRIT "$D/n.log" | tr '\n' ' ' | cut -c1-200)"
echo "upttltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
