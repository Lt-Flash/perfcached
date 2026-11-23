#!/bin/sh
# waloverruntest.sh - a WAL segment overrun takes the node out of write service (HEALING) until a
# snapshot makes the records durable again, and every acknowledged write survives a kill -9.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
SEC=ovr-client-secret
D=$(mktemp -d /var/tmp/pcovr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

ss -ltn 2>/dev/null | grep -q ":17994[[:space:]]" && {
	echo "waloverruntest: port 17994 already bound" >&2; exit 1; }

mkdir -p "$D/wal"
cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ovr-cluster-secret
[listen]
tcp = 127.0.54.4:17994
# loopback plaintext excludes perfcli's authenticated path, so this test speaks the
# native protocol on the socket (test/pcnative.py)
plaintext = loopback
[cluster]
multicast = 239.255.77.184:17284
advertise = 127.0.54.4
mode = store
collections = th
[wal]
dir = $D/wal
probe = no
fsync = everysec
# the smallest ring the config allows and no automatic snapshots: nothing is ever
# freed, so a few MB of writes must wrap it (the fault under test)
segment_mb = 1
segments = 4
save = off
# the default waits rather than lose a record; this test is about the heal
on_full = heal
# save = off stops the rules, not the pressure trigger: at 1 MB/s that snapshot takes
# seconds, so the pump wraps the ring before its marker can free a segment
rdb_mb_s = 1
# a ring big enough that it CANNOT drop: a drop fails the node by another route, and
# the dropped == 0 check below keeps the two apart
ring_kb = 16384
[collection th]
buckets_log2 = 14
EOF

echo "=== waloverruntest ==="
"$BIN" -f "$D/c.conf" > "$D/log" 2>&1 &
PID=$!
i=0; while [ $i -lt 200 ]; do grep -q "perfcached ready" "$D/log" && break
	sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/log" || { echo "daemon did not start"
	tail -5 "$D/log"; exit 1; }

sed "s|ACKED_FILE|$D/acked|" > "$D/drv.py" <<'DRIVER'
import json, pcnative, socket, time
HOST, PORT = "127.0.54.4", 17994

def call(f, m, **p):
    r = {"jsonrpc": "2.0", "id": 1, "method": m}
    if p:
        r["params"] = p
    f.write((json.dumps(r) + "\n").encode())
    f.flush()
    return json.loads(f.readline())

def state(f):
    r = call(f, "stats")
    return r.get("result", r).get("state", "ERR")

s = socket.create_connection((HOST, PORT), timeout=60)
f = pcnative.wrap(s)

# a clustered node is STARTING until the map settles
for _ in range(200):
    if state(f) == "ready":
        break
    time.sleep(0.25)
print("STATE_BEFORE", state(f))

# a watcher on its own connection: HEALING may last well under a second
import threading
seen = set(); reasons = []; stop = [False]
def watch():
    ws = pcnative.wrap(socket.create_connection((HOST, PORT), timeout=60))
    while not stop[0]:
        r = call(ws, "stats").get("result", {})
        seen.add(r.get("state"))
        if r.get("state") == "healing" and not reasons:
            reasons.append(r.get("state_reason", ""))
        time.sleep(0.02)
wt = threading.Thread(target=watch); wt.start()

# ~6 MB into a 4 MB ring that can never free a segment on its own
n = 8000; acked = []
def batch(lo, hi):
    for i in range(lo, hi):
        f.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": "set",
            "params": {"col": "th", "key": "k%06d" % i,
                       "value": "v" * 800}}) + "\n").encode())
    f.flush()
    for i in range(lo, hi):
        if (json.loads(f.readline()).get("result") or {}).get("stored"):
            acked.append("k%06d" % i)
for lo in range(0, n, 500):
    batch(lo, min(n, lo + 500))
print("DROVE", n)
print("ACKED", len(acked))
open("ACKED_FILE", "w").write("\n".join(acked))

# the overrun, the HEALING and the heal: two throttled snapshots at 1 MB/s; up to
# 180 s, and a healthy run leaves the moment it has healed
for _ in range(720):
    st = call(f, "stats").get("result", {})
    if st.get("state") == "ready" and st.get("wal", {}).get("heals", 0) >= 1:
        break
    time.sleep(0.25)
stop[0] = True; wt.join()
st = call(f, "stats").get("result", {})
print("SEEN", ",".join(sorted(x for x in seen if x)))
print("REASON_HEALING", reasons[0] if reasons else "")
print("STATE_AFTER", st.get("state"))
print("HEALS", st.get("wal", {}).get("heals", 0))
print("DROPPED", st.get("wal", {}).get("dropped", "ERR"))
print("WRITE_AFTER", json.dumps(call(f, "set", col="th", key="after", value="x")))
print("READ_AFTER", json.dumps(call(f, "get", col="th", key="k000001")))
DRIVER
python3 "$D/drv.py" > "$D/out" 2>&1
# the driver's lines are the evidence; the two JSON bodies are long and
# only wanted when something fails
grep -vE "^(WRITE|READ)_AFTER " "$D/out"

grep -q "^STATE_BEFORE ready" "$D/out" && ok "starts ready" \
	|| bad "never became ready: $(grep STATE_BEFORE "$D/out")"

grep -q "wal: FULL" "$D/log" && ok "the overrun happened (wal: FULL)" \
	|| bad "no overrun to observe - the ring never wrapped"

grep -q "^DROPPED 0" "$D/out" \
	&& ok "the ring never dropped - this is the OVERRUN path" \
	|| bad "$(grep DROPPED "$D/out") - ring drops would FAIL the node by \
the other route, so this run proves nothing about the overrun"

# out of write service, and back by itself: a heal can beat the 20 ms /stats watcher, so the log's
# NOTICE line is the evidence and the sampled state is reported beside it
HLINE=$(grep -m1 "node state ready -> healing" "$D/log")
SEEN=$(sed -n 's/^SEEN //p' "$D/out")
if [ -n "$HLINE" ]; then
	ok "the node went HEALING (its log: ready -> healing; the 20 ms watcher saw: $SEEN)"
else
	bad "never went HEALING after destroying un-snapshotted records (no ready -> healing line; watcher saw: $SEEN)"
fi
case "$HLINE" in
*"lost acknowledged writes"*)	ok "and says why: $(echo "$HLINE" | sed 's/.*healing - //' | cut -c1-70)..." ;;
*)		bad "HEALING, and the reason reads: '$(echo "$HLINE" | sed 's/.*healing//')'" ;;
esac
case ",$SEEN," in *,healing,*)
	case "$(grep '^REASON_HEALING ' "$D/out")" in
	*"lost acknowledged writes"*) ;;
	*) bad "the watcher saw HEALING with the reason '$(sed -n 's/^REASON_HEALING //p' "$D/out")'" ;;
	esac ;;
esac
H=$(sed -n 's/^HEALS //p' "$D/out")
[ "$(sed -n 's/^STATE_AFTER //p' "$D/out")" = ready ] && [ "${H:-0}" -ge 1 ] 2>/dev/null \
	&& ok "it returned to READY by itself ($H heal(s)): $(grep -m1 -o "healed after [0-9]* ms" "$D/log")" \
	|| bad "after the overrun: $(grep STATE_AFTER "$D/out"), heals ${H:-?}"

case "$(grep '^WRITE_AFTER ' "$D/out")" in
*'"stored": true'*)	ok "and takes writes again" ;;
*)		bad "a write after the heal: $(grep '^WRITE_AFTER ' "$D/out")" ;;
esac

case "$(grep '^READ_AFTER ' "$D/out")" in
*error*)	bad "reads refused - a healing node must still answer reads" ;;
*)		ok "reads answered" ;;
esac

# what it acknowledged survives - the overwritten records too (healtest covers the ring-drop path)
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
"$BIN" -f "$D/c.conf" > "$D/log2" 2>&1 &
PID=$!
i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/log2" && break
	sleep 0.1; i=$((i+1)); done
R=$(python3 - "$D/acked" <<'PY'
import json, pcnative, socket, sys, time
keys = [k for k in open(sys.argv[1]).read().split("\n") if k]
f = pcnative.wrap(socket.create_connection(("127.0.54.4", 17994), timeout=60))
def call(m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
t0 = time.time(); st0 = "?"
for _ in range(200):
    st0 = (call("stats").get("result") or {}).get("state")
    if st0 == "ready": break
    time.sleep(0.25)
tw = time.time() - t0
missing = []; why = {}
for k in keys:
    r = call("get", col="th", key=k)
    if (r.get("result") or {}).get("value") == "v" * 800: continue
    missing.append(int(k[1:]))
    e = r.get("error")
    c = "refused (%s)" % str(e.get("message", e) if isinstance(e, dict) else e)[:60] if e else \
        "absent" if (r.get("result") or {}).get("value") is None else "wrong value"
    why[c] = why.get(c, 0) + 1
rng = []
for x in missing:
    if rng and x == rng[-1][1] + 1: rng[-1][1] = x
    else: rng.append([x, x])
print("acked %d missing %d" % (len(keys), len(missing)) + ("".join(" k%06d-%06d" % tuple(r) for r in rng[:8]) if rng else "")
      + ("; state %s after %.1f s, then %s" % (st0, tw, ", ".join("%d %s" % (n, c) for c, n in why.items())) if missing else ""))
PY
)
case "$R" in "acked "*" missing 0")
	ok "after kill -9 and a restart every acknowledged write is there ($R) - the overwritten ones included";;
	*) bad "after the restart: $R"   # which snapshot and WAL the restart had, and what it loaded (114805: 365 of 6500)
	   ls -l "$D/wal" | awk 'NR > 1 {print "  ..   wal/" $9, $5}'
	   grep -E "healed|HEALING|rdb: snapshot|rdb: .*fail|torn" "$D/log" | cut -c1-170 | sed 's/^/  ..   run 1: /'
	   grep -E "rdb|recover|replay|snapshot|marker|torn|segment" "$D/log2" | head -12 | cut -c1-170 | sed 's/^/  ..   restart: /';; esac

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "waloverruntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
