#!/bin/sh
# relayinteresttest.sh - PS12: a node set to `pubsub_relay = interested` is
# relayed only what may match its subscriptions.  Three nodes: 1 and 2
# interested, 3 on `all`.  Exact channels and patterns are filtered, a node
# on `all` still gets everything, loss stays exact on filtered peers, a new
# subscription is relayed at once, a restarted sender or receiver resyncs,
# and a channel nobody holds any more stops being relayed after the rebuild.
# Fail-first: the PS11 build refuses `pubsub_relay` (unknown key).
# Usage: test/relayinteresttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrelayint.XXXXXX); P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n> <all|interested>
	mkdir -p "$D/s$1"; cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = relayint-client-secret
cluster = relayint-cluster-secret
[listen]
tcp = 127.0.0.1:1758$1
resp = 127.0.0.1:1759$1
http = 127.0.0.1:1858$1
plaintext = loopback
[cluster]
multicast = 239.255.77.54:17163
advertise = 127.0.16.$1
mode = eager
collections = 0
pubsub_relay = $2
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/n$1.conf"; }
start() { mv "$D/n$1.log" "$D/n$1.$(date +%s%N).log" 2>/dev/null
	"$BIN" -f "$D/n$1.conf" -D > "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 did not start:"; tail -3 "$D/n$1.log"; return 1; }
stop() { eval "p=\$P$1"; kill "$p"; i=0; while kill -0 "$p" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done; eval "P$1="; }
stat() { curl -s -m 3 "http://127.0.0.1:1858$1/stats"; }

# ---- the setting ---------------------------------------------------------------
node 1 interested
"$BIN" -E -f "$D/n1.conf" > "$D/dump1" 2>&1
grep -q "^pubsub_relay = interested" "$D/dump1" && ok "the config dump prints pubsub_relay = interested" || bad "dump: $(grep -i pubsub "$D/dump1" | tr '\n' ' ')"
node 9 sometimes
"$BIN" -E -f "$D/n9.conf" > "$D/dump9" 2>&1 && bad "pubsub_relay = sometimes was accepted" \
	|| { grep -q "pubsub_relay must be all or interested" "$D/dump9" && ok "any other value is refused, naming both choices" || bad "refusal text: $(tail -2 "$D/dump9" | tr '\n' ' ')"; }

# ---- the fleet -------------------------------------------------------------------
node 1 interested; node 2 interested; node 3 all
start 1 && start 2 && start 3 || { echo "relayinteresttest: $pass passed, $((fail+1)) failed"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	s=""; for n in 1 2 3; do s="$s$(stat $n | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"], end="|")' 2>/dev/null)"; done
	[ "$s" = "ready 2|ready 2|ready 2|" ] && break; sleep 0.2; i=$((i+1)); done
[ "$s" = "ready 2|ready 2|ready 2|" ] && ok "three nodes formed a fleet" || { bad "fleet never formed: $s"; echo "relayinteresttest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
export RESFILE="$RES" D
cat > "$D/lib.py" <<'PY'
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m):
    res.write(("P " if c else "F ") + m + "\n"); res.flush()
def get(n, path): return json.load(urllib.request.urlopen("http://127.0.0.1:1858%d%s" % (n, path), timeout=5))
def interest(frm, to):
    m = {x["addr"]: x for x in get(frm, "/members")["members"]}.get("127.0.16.%d" % to, {})
    return m.get("relay", {}).get("interest")
def ps(n): return get(n, "/stats")["pubsub"]
def wait(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        try:
            if pred(): return True
        except Exception:
            pass
        time.sleep(0.1)
    return False
class R:
    def __init__(self, n):
        self.s = socket.create_connection(("127.0.0.1", 17590 + n), timeout=10); self.f = self.s.makefile("rb")
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    def sub(self, verb, name):
        self.send(verb, name)
        for _ in range(6): self.f.readline()      # *3 $n verb $n name :count
    def pub(self, chan, count):
        self.s.sendall(("*3\r\n$7\r\nPUBLISH\r\n$%d\r\n%s\r\n$3\r\nhi!\r\n" % (len(chan), chan) * count).encode())
        for _ in range(count): self.f.readline()
    def pub_mixed(self, chans, rounds):
        one = "".join("*3\r\n$7\r\nPUBLISH\r\n$%d\r\n%s\r\n$3\r\nhi!\r\n" % (len(c), c) for c in chans)
        self.s.sendall((one * rounds).encode())
        for _ in range(len(chans) * rounds): self.f.readline()
PY

run() { RESFILE="$RES" timeout 120 python3 -c "import sys; sys.path.insert(0, '$D'); from lib import *; $1" 2>> "$D/py.err"; }
run '
ok = wait(lambda: interest(3, 1) == "filtered" and interest(3, 2) == "filtered" and interest(1, 2) == "filtered" and interest(1, 3) == "all" and interest(2, 3) == "all", 15)
check(ok, "each node filters relays to the interested nodes and sends everything to the node on all (3->1 %s, 3->2 %s, 1->3 %s)" % (interest(3, 1), interest(3, 2), interest(1, 3)))
s1, s3 = ps(1), ps(3)
check(s1["relay_mode"] == "interested" and int(s1["interest_version"], 16) >> 48 != 0 and s3["relay_mode"] == "all" and int(s3["interest_version"], 16) == 0,
      "/stats: relay_mode and interest_version say who asked to be filtered (%s, %s)" % (s1["interest_version"], s3["interest_version"]))
'

# the subscribers live in one long python process, so they stay subscribed
cat > "$D/main.py" <<'PY'
import sys, os
sys.path.insert(0, os.environ["D"])
from lib import *
s1, s2, s3 = R(1), R(2), R(3)
s1.sub("SUBSCRIBE", "news"); s2.sub("PSUBSCRIBE", "sport.*"); s3.sub("SUBSCRIBE", "news")
time.sleep(0.5)
b = {n: ps(n) for n in (1, 2, 3)}
pub3 = R(3)
# interleaved: every relay a filtered peer IS sent follows ones it was not,
# so counters shared with the other peers would show up as losses
pub3.pub_mixed(["news", "sport.x", "other"], 100)
d = lambda a, n, k: a[n][k] - b[n][k]
wait(lambda: ps(1)["delivered"] - b[1]["delivered"] >= 100 and ps(2)["delivered"] - b[2]["delivered"] >= 100, 5)
time.sleep(0.5)
a = {n: ps(n) for n in (1, 2, 3)}
check(d(a, 1, "relay_recv") == 100 and d(a, 1, "delivered") == 100,
      "node 1 (SUBSCRIBE news) is relayed news and nothing else (%d relayed)" % d(a, 1, "relay_recv"))
check(d(a, 2, "relay_recv") == 100 and d(a, 2, "delivered") == 100,
      "node 2 (PSUBSCRIBE sport.*) is relayed sport.x and nothing else (%d relayed)" % d(a, 2, "relay_recv"))
check(d(a, 3, "relay_skipped") == 400, "node 3 counted the 400 relays it did not send (%d)" % d(a, 3, "relay_skipped"))
check(d(a, 1, "relay_lost") == 0 and d(a, 2, "relay_lost") == 0,
      "a filtered peer counts nothing it was not sent as lost (lost %d, %d)" % (d(a, 1, "relay_lost"), d(a, 2, "relay_lost")))

b = a
pub1 = R(1)
pub1.pub("news", 50); pub1.pub("zzz", 10)
wait(lambda: ps(3)["relay_recv"] - b[3]["relay_recv"] >= 60, 5)
time.sleep(0.5)
a = {n: ps(n) for n in (1, 2, 3)}
check(d(a, 3, "relay_recv") == 60, "node 3, on all, is relayed everything node 1 publishes (%d of 60)" % d(a, 3, "relay_recv"))
check(d(a, 2, "relay_recv") == 0 and d(a, 1, "relay_skipped") == 60,
      "node 2 is relayed none of it (%d), node 1 counted 60 skipped (%d)" % (d(a, 2, "relay_recv"), d(a, 1, "relay_skipped")))

# a first subscription is relayed at once, not after a heartbeat
b = a
s2.sub("SUBSCRIBE", "late")
time.sleep(0.1)
pub3.pub("late", 20)
wait(lambda: ps(2)["delivered"] - b[2]["delivered"] >= 20, 3)
a = {n: ps(n) for n in (1, 2, 3)}
check(d(a, 2, "delivered") == 20, "a subscription on node 2 is relayed 100 ms later (%d of 20)" % d(a, 2, "delivered"))
check(d(a, 3, "interest_updates_received") >= 1, "carried by an update, not a resync (%d updates)" % d(a, 3, "interest_updates_received"))

open(os.path.join(os.environ["D"], "phase1"), "w").write("done")
# hold the subscribers while the shell restarts node 3
while not os.path.exists(os.path.join(os.environ["D"], "phase2")):
    time.sleep(0.1)
b = {n: ps(n) for n in (1, 2)}
pub3 = R(3)
s1b = R(1); s1b.sub("SUBSCRIBE", "missed")
time.sleep(0.2)
pub3.pub("missed", 20); pub3.pub("other", 20)
wait(lambda: ps(1)["delivered"] - b[1]["delivered"] >= 20, 3)
time.sleep(0.5)
a = {n: ps(n) for n in (1, 2)}
check(d(a, 1, "delivered") == 20 and d(a, 1, "relay_recv") == 20,
      "after node 3 restarted, node 1 is relayed its new channel and not other (%d relayed)" % d(a, 1, "relay_recv"))
check(ps(3)["interest_resyncs_received"] >= 2, "the restarted node took both interested peers' full state (%d)" % ps(3)["interest_resyncs_received"])

# a channel nobody on node 1 holds any more stops, after the rebuild
b = {n: ps(n) for n in (1, 3)}
s1.send("UNSUBSCRIBE", "news"); s1.f.readline()
ok = wait(lambda: ps(3)["interest_resyncs_received"] > b[3]["interest_resyncs_received"], 10)
time.sleep(0.3)
b = {n: ps(n) for n in (1, 3)}
pub3.pub("news", 10); pub3.pub("missed", 10)
wait(lambda: ps(1)["relay_recv"] - b[1]["relay_recv"] >= 10, 3)
time.sleep(0.5)
a = {n: ps(n) for n in (1, 3)}
check(ok and d(a, 1, "relay_recv") == 10,
      "after UNSUBSCRIBE news, node 1 rebuilds, node 3 resyncs, and only missed is relayed (resync %s, %d relayed)" % (ok, d(a, 1, "relay_recv")))
open(os.path.join(os.environ["D"], "phase3"), "w").write("done")
while not os.path.exists(os.path.join(os.environ["D"], "phase4")):
    time.sleep(0.1)
PY
D="$D" RESFILE="$RES" timeout 150 python3 "$D/main.py" 2>> "$D/py.err" & PY=$!
i=0; while [ ! -f "$D/phase1" ] && kill -0 $PY 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
# ---- the sender restarts ----------------------------------------------------------------
stop 3; start 3 || bad "node 3 did not restart"
i=0; while [ $i -lt 100 ]; do
	r=$(curl -s -m 3 http://127.0.0.1:18583/members | python3 -c 'import json,sys
m={x["addr"]: x for x in json.load(sys.stdin)["members"]}
print(m.get("127.0.16.1",{}).get("relay",{}).get("interest"), m.get("127.0.16.2",{}).get("relay",{}).get("interest"))' 2>/dev/null)
	[ "$r" = "filtered filtered" ] && break; sleep 0.2; i=$((i+1)); done
[ "$r" = "filtered filtered" ] && ok "a restarted node 3 filters to both interested peers again" || bad "node 3 after restart sees: $r"
: > "$D/phase2"
i=0; while [ ! -f "$D/phase3" ] && kill -0 $PY 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
: > "$D/phase4"; wait $PY 2>/dev/null

# ---- a receiver restarts: its old patterns go with it -------------------------------------
stop 2; start 2 || bad "node 2 did not restart"
run '
ok = wait(lambda: interest(3, 2) == "filtered", 15)
s2 = R(2); s2.sub("SUBSCRIBE", "after")
time.sleep(0.3)
b = ps(2)
p = R(3); p.pub("after", 10); p.pub("sport.x", 10)
wait(lambda: ps(2)["delivered"] - b["delivered"] >= 10, 3)
time.sleep(0.5)
a = ps(2)
check(ok and a["relay_recv"] - b["relay_recv"] == 10,
      "a restarted node 2 is relayed its new channel, not its old process pattern sport.* (%d relayed)" % (a["relay_recv"] - b["relay_recv"]))
m = urllib.request.urlopen("http://127.0.0.1:18583/metrics", timeout=5).read().decode()
check("perfcached_pubsub_relay_skipped_total" in m and "perfcached_pubsub_relay_peers_filtered 2" in m and "perfcached_pubsub_interest_resyncs_total{dir=\"received\"}" in m,
      "/metrics carries skipped relays, filtered peers and resyncs")
'

while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -5 "$D/py.err"; }
grep -h "ERROR\|CRIT" "$D"/n*.log | grep -v "ERROR: .*n9.conf" | head -3 | while IFS= read -r l; do echo "  log: $l"; done
echo "relayinteresttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
