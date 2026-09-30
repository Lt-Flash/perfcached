#!/bin/sh
# waloverruntest.sh - a segment overrun takes the node out of write
# service, and (S229) it heals itself.
#
# A ring drop and a segment overrun are the same event: an acknowledged
# write the LOG no longer holds.  The overrun used to overwrite the
# records, count them, and leave the node in service; then it FAILED the
# node until a restart, and the overwritten records were gone after it.
# Since S229 the node goes HEALING (writes refused, reads served) and takes
# a snapshot: the records are still in memory - every write stores to the
# table before the WAL - so the snapshot makes them durable again, and the
# node returns to READY by itself.  This drives a real overrun and asserts
# all of that, and then that every acknowledged write survives a kill -9.
# Usage: test/waloverruntest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=ovr-client-secret
D=$(mktemp -d /var/tmp/pcovr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
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
# loopback plaintext: perfcli's authenticated path is exclusive with it,
# so this test speaks JSON-RPC on the socket rather than through the CLI
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
# The smallest ring the config allows, and no automatic snapshots: the
# marker never advances, so nothing is ever free and a few MB of writes
# must wrap it.  This is the fault being reproduced, not a suggestion.
segment_mb = 1
segments = 4
save = off
# A ring big enough that it CANNOT drop: this test is about segments
# filling faster than snapshots free them, and a ring drop already
# FAILS the node by a different route.  The assertion below on
# dropped == 0 is what keeps the two apart - without it this test
# passes on a binary that never learned to shed an overrun at all.
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
import json, socket, time
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
f = s.makefile("rwb")

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
    ws = socket.create_connection((HOST, PORT), timeout=60).makefile("rwb")
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

for _ in range(120):
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

# S229: out of write service, and back by itself
case ",$(sed -n 's/^SEEN //p' "$D/out")," in *,healing,*)
	ok "the node went HEALING (states seen: $(sed -n 's/^SEEN //p' "$D/out"))";;
	*) bad "never seen HEALING after destroying un-snapshotted records (states seen: $(sed -n 's/^SEEN //p' "$D/out"))";; esac
case "$(grep '^REASON_HEALING ' "$D/out")" in
*"lost acknowledged writes"*)	ok "and says why: $(sed -n 's/^REASON_HEALING //p' "$D/out" | cut -c1-70)..." ;;
*)		bad "HEALING, and the reason reads: '$(sed -n 's/^REASON_HEALING //p' "$D/out")'" ;;
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

# what it acknowledged survives - the overwritten records too.  Not a
# fail-first for S229: a build before it also kept them, because the
# overrun path has always requested a snapshot (it only never came back
# to service).  The ring-DROP path is where acknowledged writes were
# really lost before S229 - healtest shows that one (6,192 of them).
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
"$BIN" -f "$D/c.conf" > "$D/log2" 2>&1 &
PID=$!
i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/log2" && break
	sleep 0.1; i=$((i+1)); done
R=$(python3 - "$D/acked" <<'PY'
import json, socket, sys, time
keys = [k for k in open(sys.argv[1]).read().split("\n") if k]
f = socket.create_connection(("127.0.54.4", 17994), timeout=60).makefile("rwb")
def call(m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
for _ in range(200):
    if (call("stats").get("result") or {}).get("state") == "ready": break
    time.sleep(0.25)
miss = sum(1 for k in keys if (call("get", col="th", key=k).get("result") or {}).get("value") != "v" * 800)
print("acked %d missing %d" % (len(keys), miss))
PY
)
case "$R" in "acked "*" missing 0")
	ok "after kill -9 and a restart every acknowledged write is there ($R) - the overwritten ones included";;
	*) bad "after the restart: $R";; esac

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "waloverruntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
