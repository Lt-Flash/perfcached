#!/bin/sh
# fwdtest.sh — the forwarded-write plane under a DEEP PIPELINE.
# 100k x 256B sets stream through a non-holder ingress with 400 in
# flight; placement forwards them to the holder.  The regression this
# encodes (found live): park-table exhaustion answered every verb with
# the PULL-miss shape - a SET got {"found":false} while the already-
# sent forward STORED at the holder anyway (client told failure, write
# landed).  Asserted here:
#  - every reply to a set carries "stored" - no foreign shapes ever;
#  - TRUTH CONSERVATION: fleet entries == the stored:true count exactly
#    (every confirmed write is really there, every refusal really
#    absent - the old bug stored 100% while confirming 93%);
#  - refusals are RARE (<=1%): the datagram plane may drop a burst
#    (that is the v1 contract - an honest stored:false is retryable),
#    but systemic refusal means the plane is broken.
# Usage: test/fwdtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfw.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
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
ring_kb = 8192   # the crash-durability check needs the whole forward
                 # burst IN the WAL: an overflowed ring drops sequenced
                 # records (counted, by design) and the sync barrier
                 # then honestly syncs the survivors - on a starved
                 # 2-vCPU CI box that read as data loss
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
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/n1.log" && \
		grep -q "perfcached ready" "$D/n2.log" && break
	sleep 0.1; i=$((i+1))
done
sleep 3.5   # membership

python3 - <<'EOF'
import json, socket, sys, time
s = socket.create_connection(("127.0.0.1", 17041), timeout=60)
f = s.makefile("rwb")
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
time.sleep(2)
def ent(port):
    g = socket.create_connection(("127.0.0.1", port), timeout=5).makefile("rwb")
    g.write(b'{"jsonrpc":"2.0","id":1,"method":"stats","params":{"col":"px"}}\n')
    g.flush()
    return json.loads(g.readline())["result"]["collections"][0]["entries"]
a, b = ent(17041), ent(17042)
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

# ---- forwarded writes are DURABLE at the holder -------------------------
# (the ring-count bug: the peer thread silently dropped every WAL
#  record for forwarded stores - rings were sized workers+3 while the
#  peer thread is slot workers+4)
B0=$(python3 - <<'EOF'
import json, socket
g = socket.create_connection(("127.0.0.1", 17042), timeout=5).makefile("rwb")
g.write(b'{"jsonrpc":"2.0","id":1,"method":"sync"}\n'); g.flush()
r = json.loads(g.readline())
assert r["result"]["synced"], r
g.write(b'{"jsonrpc":"2.0","id":2,"method":"stats","params":{"col":"px"}}\n')
g.flush()
s = json.loads(g.readline())["result"]
print(s["collections"][0]["entries"], s.get("wal", {}).get("dropped", -1))
EOF
)
set -- $B0; B0=$1; WDROP=$2
# the equality below asserts crash-durability of every SEQUENCED write;
# a ring drop is a sequenced write the barrier can never cover, so it
# must be its own loud failure, not a misattributed "lost in crash"
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
import json, socket
g = socket.create_connection(("127.0.0.1", 17042), timeout=5).makefile("rwb")
g.write(b'{"jsonrpc":"2.0","id":1,"method":"stats","params":{"col":"px"}}\n')
g.flush()
print(json.loads(g.readline())["result"]["collections"][0]["entries"])
EOF
)
echo "holder kill -9 recovery: $B0 forwarded entries before, $B1 after"
[ "$B1" = "$B0" ] && ok || bad "forwarded writes lost in crash ($B0 -> $B1)"

kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
echo "fwdtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
