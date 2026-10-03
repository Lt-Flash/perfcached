#!/bin/sh
# eagermisstest.sh - S303: in EAGER mode a local miss is the answer; in
# STORE mode a miss asks the fleet and keeps what it finds.
#
# Eager replicates every record to every node, so a READY node's miss is
# the fleet's.  It used to ask anyway: every miss parked the client for a
# pull round trip, and a slow peer cost the 80 ms pull timeout (245-247:
# 0 of 67,000 such pulls ever found the key).  Store mode is the
# opposite: a record lives where it was written, so a miss must ask,
# and the copy it pulls is kept, so the next read is local.
#   1  eager: a record written on node 2 reads on node 1 (replicated),
#      and so does one written by `mset` within a second (pushed, not
#      left to the ~10 s sweep - mset did not push before S303); just
#      formed, node 1 is not yet settled (eager_miss_final false: for
#      30 s after a start or a peer coming back a miss still asks), and
#      once settled 50 missing keys read through it all miss and send
#      NO pull
#   2  store: a record written on node 2 only reads through node 1 (one
#      pull), is then HELD on node 1, and a second read sends no pull;
#      a missing key still asks (one pull)
# The mode is declared fleet-wide ([cluster] mode + collections), as
# 245-247 run it: that is what turns pulls on - a per-collection mode
# alone does not, and a test written that way sees no pulls at all.
# Fail-first: before S303 part 1 sends one pull per missing key (50).
# Usage: test/eagermisstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcem.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

fleet() { # fleet <tag> <mode> <port base> <group octet>: three nodes
	for n in 1 2 3; do
		cat > "$D/$1$n.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1$n
[memory]
arena_mb = 32
[secrets]
client = em-client-secret
cluster = em-cluster-secret
[listen]
tcp = 127.0.0.1:$(($3 + n))
http = 127.0.0.1:$(($3 + n + 5))
plaintext = loopback
[cluster]
multicast = 239.255.77.$4:$(($3 + 9))
advertise = 127.0.$4.$n
mode = $2
collections = 0
[collection 0]
buckets_log2 = 10
EOF
		chmod 600 "$D/$1$n.conf"; mkdir -p "$D/s$1$n"
		"$BIN" -f "$D/$1$n.conf" > "$D/$1$n.log" 2>&1 &
		PIDS="$PIDS $!"
	done
}

python_part() { # python_part <part> <port base>
	python3 - "$1" "$2" <<'PY'
import json, socket, sys, time, urllib.request
part, base = sys.argv[1], int(sys.argv[2])
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)
def stats(n):
    return json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (base + n + 5), timeout=5).read())
def live(n):
    r = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/members" % (base + n + 5), timeout=5).read())
    return sum(1 for m in r.get("members", []) if m.get("gone_s", -1) < 0)
class Node:
    def __init__(self, n):
        s = socket.create_connection(("127.0.0.1", base + n), 5); self.f = s.makefile("rwb")
    def call(self, method, **p):
        self.f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": p}).encode() + b"\n")
        self.f.flush()
        r = json.loads(self.f.readline())
        return r.get("result", r)
def pulls(n):
    return stats(n)["cluster"].get("pull_sent", 0)
def entries(n):
    return [c["entries"] for c in stats(n)["collections"] if c["name"] == "0"][0]

t = time.time()
while time.time() - t < 30:
    try:
        if all(live(n) == 3 and stats(n).get("state") == "ready" for n in (1, 2, 3)):
            break
    except Exception:
        pass
    time.sleep(0.5)
else:
    bad("the %s fleet did not form ready" % part)
    print("RESULT %d %d" % (pass_n, fail_n)); sys.exit(0)

n1, n2 = Node(1), Node(2)
if part == "eager":
    early = stats(1)["cluster"].get("eager_miss_final")
    if early is False:
        ok("just formed, node 1 is not settled yet - its misses still ask (eager_miss_final false)")
    else:
        bad("just formed, eager_miss_final reads %s - want false for 30 s" % early)
    n2.call("set", col="0", key="here", value="v-here")
    got = None
    for _ in range(40):
        got = n1.call("get", col="0", key="here")
        if isinstance(got, dict) and got.get("found"):
            break
        time.sleep(0.1)
    if isinstance(got, dict) and got.get("value") == "v-here":
        ok("a record written on node 2 reads on node 1 (replicated)")
    else:
        bad("the replicated record did not read on node 1: %s" % got)
    n2.call("mset", col="0", items=[{"key": "m1", "value": "v-m1"}, {"key": "m2", "value": "v-m2"}])
    got = None
    for _ in range(10):
        got = [n1.call("get", col="0", key=k).get("value") for k in ("m1", "m2")]
        if got == ["v-m1", "v-m2"]:
            break
        time.sleep(0.1)
    if got == ["v-m1", "v-m2"]:
        ok("an mset on node 2 reads on node 1 within a second (pushed)")
    else:
        bad("an mset on node 2 did not reach node 1 within a second: %s" % got)
    t = time.time()
    while time.time() - t < 50 and not stats(1)["cluster"].get("eager_miss_final"):
        time.sleep(1)
    if stats(1)["cluster"].get("eager_miss_final"):
        ok("settled after %.0f s more: eager_miss_final true" % (time.time() - t))
    else:
        bad("node 1 never settled (eager_miss_final still false after 50 s)")
    p0 = pulls(1)
    misses = sum(1 for i in range(50) if not n1.call("get", col="0", key="absent-%d" % i).get("found"))
    p1 = pulls(1)
    print("    50 missing keys through node 1: %d missed, pulls sent %d -> %d" % (misses, p0, p1))
    if misses == 50:
        ok("every missing key answered as a miss")
    else:
        bad("%d of 50 missing keys did not miss" % misses)
    if p1 == p0:
        ok("no pull sent - an eager miss is final")
    else:
        bad("%d pulls sent for 50 eager misses - the node still asks the fleet" % (p1 - p0))
else:
    n2.call("set", col="0", key="there", value="v-there")
    time.sleep(0.5)
    e0, p0 = entries(1), pulls(1)
    got = n1.call("get", col="0", key="there")
    e1, p1 = entries(1), pulls(1)
    got2 = n1.call("get", col="0", key="there")
    p2 = pulls(1)
    print("    node 1: entries %d -> %d, pulls %d -> %d -> %d" % (e0, e1, p0, p1, p2))
    if got.get("value") == "v-there" and p1 == p0 + 1:
        ok("a record held only on node 2 reads through node 1 with one pull")
    else:
        bad("first read: %s, pulls %d -> %d" % (got, p0, p1))
    if e1 == e0 + 1 and got2.get("value") == "v-there" and p2 == p1:
        ok("node 1 kept the pulled copy - the second read sends no pull")
    else:
        bad("entries %d -> %d, second read %s, pulls %d -> %d" % (e0, e1, got2, p1, p2))
    p3 = pulls(1)
    m = n1.call("get", col="0", key="nowhere")
    p4 = pulls(1)
    if not m.get("found") and p4 == p3 + 1:
        ok("a missing key still asks the fleet in store mode (one pull)")
    else:
        bad("store miss: %s, pulls %d -> %d" % (m, p3, p4))
print("RESULT %d %d" % (pass_n, fail_n))
PY
}

tally() { # add a python part's RESULT line to the counts
	r=$(grep "^RESULT" "$1" | tail -1)
	grep -v "^RESULT" "$1"
	pass=$((pass + $(echo "$r" | cut -d' ' -f2)))
	fail=$((fail + $(echo "$r" | cut -d' ' -f3)))
}

echo "--- 1: eager - a local miss is final"
fleet e eager 17870 87
python_part eager 17870 > "$D/p1.out" 2>&1; tally "$D/p1.out"
for p in $PIDS; do kill $p 2>/dev/null; done; for p in $PIDS; do wait $p 2>/dev/null; done; PIDS=""

echo "--- 2: store - a miss asks, and what it pulls is kept"
fleet t store 17860 86
python_part store 17860 > "$D/p2.out" 2>&1; tally "$D/p2.out"

echo "eagermisstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
