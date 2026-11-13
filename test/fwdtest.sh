#!/bin/sh
# fwdtest.sh - forwarded writes under a deep pipeline: every reply carries "stored", the fleet holds exactly the
# confirmed writes, refusals stay under 1%, forwards are durable.  Usage: test/fwdtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcfw.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport> <arena>  (advertises 127.0.0.3<n>)
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = $3
[secrets]
client = fw-client-secret
cluster = fw-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[wal]
dir = $D/wal$1
fsync = everysec
segment_mb = 64
segments = 4
ring_kb = ${FWD_RING_KB:-8192}
                 # the crash-durability check needs the whole forward burst in the WAL: an overflowed ring
                 # drops records; FWD_RING_KB=1024 under 'taskset -c 0,1' shows the drop check firing
[cluster]
multicast = 239.255.77.35:17135
advertise = 127.0.0.3$1
pull_timeout_ms = 300
[collection px]
buckets_log2 = 16
mode = proxy
EOF
}
node 1 17041 32
node 2 17042 64
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 &
P2=$!
# wait for the doors themselves, not "perfcached ready": the nodes log at warn and that line is a
# notice
RDY=300; [ -n "$SANRT" ] && RDY=900
up() { python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1).close()" "$1" 2>/dev/null; }
i=0
while [ $i -lt $RDY ]; do
	up 17041 && up 17042 && break
	sleep 0.1; i=$((i+1))
done
if ! up 17041 || ! up 17042; then
	echo "FAIL: the nodes' doors were not open after $((RDY / 10)) s"
	for n in 1 2; do echo "--- node $n log (alive: $(eval kill -0 \$P$n 2>/dev/null && echo yes || echo NO))"; tail -15 "$D/n$n.log"; done
	exit 1
fi
# a joiner is CONNECTING until its links answer: wait for both ready and seeing each other
st() { python3 -c 'import json,pcnative,socket,sys
g = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5))
g.write(b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"stats\"}\n"); g.flush()
r = json.loads(g.readline())["result"]; print(r.get("state"), r["cluster"].get("peers_up"))' "$1" 2>/dev/null; }
i=0
while [ $i -lt $RDY ] && { [ "$(st 17041)" != "ready 1" ] || [ "$(st 17042)" != "ready 1" ]; }; do
	sleep 0.1; i=$((i+1))
done

# conservation is judged on the fleet once moves are done: the rebalancer sheds the holder's
# records at once and a record in flight is on neither node, so poll up to the settle budget
FIRST_SETTLE=60; [ -n "$SANRT" ] && FIRST_SETTLE=180
python3 - "$FIRST_SETTLE" <<'EOF'
import json, pcnative, socket, sys, time
s = socket.create_connection(("127.0.0.1", 17041), timeout=60)
f = pcnative.wrap(s)
val = "V" * 256; n = 100000; acked = 0
stored = 0; refused = 0; foreign = 0; errors = 0; sample = None
def take(r):
    global stored, refused, foreign, errors, sample
    res = r.get("result")
    if res is None:
        errors += 1
        if sample is None: sample = r
    elif "stored" in res:
        if res["stored"]: stored += 1
        else: refused += 1
    else:
        foreign += 1
        if sample is None: sample = r
t0 = time.time()
for i in range(n):
    f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"px","key":"k%06d"%i,"value":val}}).encode()+b"\n")
    if i - acked >= 400:
        f.flush()
        while i - acked >= 200:
            take(json.loads(f.readline())); acked += 1
f.flush()
while acked < n:
    take(json.loads(f.readline())); acked += 1
dt = time.time() - t0
print("pipeline drained in %.1fs: stored=%d refused=%d foreign-shape=%d errors=%d"
    % (dt, stored, refused, foreign, errors))
if sample: print("first bad reply:", json.dumps(sample)[:160])
def st(port):
    g = pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=5))
    g.write(b'{"jsonrpc":"2.0","id":1,"method":"stats","params":{"col":"px"}}\n')
    g.flush()
    r = json.loads(g.readline())["result"]
    return r["collections"][0]["entries"], (r["cluster"].get("rebalance") or {}).get("state", "?")
time.sleep(2)
deadline = time.time() + float(sys.argv[1]); first = None
while True:
    (a, ra), (b, rb) = st(17041), st(17042)
    if first is None:
        first = (a, b, ra, rb)
    if a + b == stored or time.time() >= deadline:
        break
    time.sleep(1)
if first[0] + first[1] != a + b:
    print("entries 2 s after the drain: A=%d B=%d total=%d (rebalance %s/%s) - moves in flight"
        % (first[0], first[1], first[0] + first[1], first[2], first[3]))
print("entries: A=%d B=%d total=%d" % (a, b, a + b))
rc = 0
if foreign or errors: rc = 1
elif a + b != stored: rc = 2         # truth conservation, exact
elif stored + refused != n or refused > n // 100: rc = 3
sys.exit(rc)
EOF
case $? in
	0) ok; ok; ok ;;
	1) bad "foreign-shaped or error replies to set"; ok; ok ;;
	2) bad "entries != confirmed stores (a write lied)" ;;
	3) bad "refusals excessive or unaccounted" ;;
	*) bad "loader failed" ;;
esac

# ---- forwarded writes are durable at the holder.  The rebalancer moves records while the holder
# sits above the fleet mean: let it settle before the barrier, judge on the fleet after the restart.
SETTLE=60; [ -n "$SANRT" ] && SETTLE=180
B0=$(python3 - "$SETTLE" <<'EOF'
import json, pcnative, socket, sys, time
g = pcnative.wrap(socket.create_connection(("127.0.0.1", 17042), timeout=5))
def stats():
    g.write(b'{"jsonrpc":"2.0","id":2,"method":"stats","params":{"col":"px"}}\n'); g.flush()
    return json.loads(g.readline())["result"]
deadline = time.time() + float(sys.argv[1])
s = stats(); rb = s["cluster"].get("rebalance") or {}
while rb.get("state") != "settled" and time.time() < deadline:
    time.sleep(0.5); s = stats(); rb = s["cluster"].get("rebalance") or {}
g.write(b'{"jsonrpc":"2.0","id":1,"method":"sync"}\n'); g.flush()
r = json.loads(g.readline())
assert r["result"]["synced"], r
s = stats(); rb = s["cluster"].get("rebalance") or {}
print(s["collections"][0]["entries"], s.get("wal", {}).get("dropped", -1), rb.get("state", "?"), rb.get("moved_records", -1))
EOF
)
set -- $B0; B0=$1; WDROP=$2; RBSTATE=$3; RBMOVED=$4
[ "$RBSTATE" = settled ] || echo "    note: the rebalancer did not settle within $SETTLE s (state $RBSTATE, moved $RBMOVED) - the holder may still be shedding"
# crash-durability covers every sequenced write; a ring drop is one the barrier can never cover,
# so it fails loudly on its own
[ "$WDROP" = "0" ] && ok || bad "wal ring dropped $WDROP records pre-barrier (ring too small for this host)"
kill -9 $P2 2>/dev/null; P2=
sleep 0.5
"$BIN" -f "$D/n2.conf" >> "$D/n2.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 200 ]; do
	python3 -c "import socket; socket.create_connection((\"127.0.0.1\", 17042), timeout=0.3).close()" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
B1=$(python3 - <<'EOF'
import json, pcnative, socket
def ent(port):
    g = pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=5))
    g.write(b'{"jsonrpc":"2.0","id":1,"method":"stats","params":{"col":"px"}}\n'); g.flush()
    return json.loads(g.readline())["result"]["collections"][0]["entries"]
print(ent(17042), ent(17041))
EOF
)
set -- $B1; B1=$1; A1=$2
echo "holder kill -9 recovery: $B0 forwarded entries before, $B1 after; fleet A+B = $((A1 + B1)) of 100000"
if [ $((A1 + B1)) -lt 100000 ]; then
	bad "forwarded writes lost in crash (fleet $((A1 + B1)) < 100000; holder $B0 -> $B1)"
elif [ "$B1" != "$B0" ]; then
	echo "    note: the holder's count moved $B0 -> $B1 with the fleet intact - a rebalancer move landed between the barrier and the kill (state at the barrier: $RBSTATE)"
	ok
else
	ok
fi

kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
echo "fwdtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
