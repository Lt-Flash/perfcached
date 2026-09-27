#!/bin/sh
# relayrxtest.sh - PS11: pub/sub relays arrive on a port of their own, read by
# several threads, off the cluster socket.  A sender uses a peer's relay port
# only after the peer answered a probe there; a peer without one, or whose
# port is taken, keeps receiving on the cluster socket; nothing is lost or
# duplicated either way.
# Fail-first: 0.4.0-rc1 has no relay port - no `relay` on /members, no
# relay_rx_threads on /stats, no pubsub_port in the config dump.
# Usage: test/relayrxtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrelayrx.XXXXXX); P1= P2= P3= P9= PY=
trap 'for v in "$P1" "$P2" "$P3" "$P9" "$PY"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n> <rx threads>
	mkdir -p "$D/s$1"; cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = relayrx-client-secret
cluster = relayrx-cluster-secret
[listen]
tcp = 127.0.0.1:1756$1
resp = 127.0.0.1:1757$1
http = 127.0.0.1:1856$1
plaintext = loopback
[cluster]
multicast = 239.255.77.53:17153
advertise = 127.0.15.$1
mode = eager
collections = 0
pubsub_rx_threads = $2
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/n$1.conf"; }
start() { "$BIN" -f "$D/n$1.conf" -D > "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 did not start:"; tail -3 "$D/n$1.log"; return 1; }
stat() { curl -s -m 3 "http://127.0.0.1:1856$1/stats"; }

# ---- one node: the config dump, and a relay port somebody else holds ------
node 9 2
"$BIN" -E -f "$D/n9.conf" > "$D/dump9" 2>&1
grep -q "^pubsub_port = 17154" "$D/dump9" && grep -q "^pubsub_rx_threads = 2" "$D/dump9" \
	&& ok "the config dump prints the relay port (cluster port + 1) and its threads" \
	|| bad "config dump: $(grep -i pubsub "$D/dump9" | tr '\n' ' ')"
python3 -c 'import socket,time; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.15.9", 17154)); time.sleep(60)' &
PY=$!
sleep 0.5
start 9 || exit 1
sleep 1
grep -q "pub/sub relay port 127.0.15.9:17154 is taken" "$D/n9.log" \
	&& ok "a relay port held by another process is refused, and the log says so" \
	|| bad "no 'taken' line: $(grep -i relay "$D/n9.log" | tail -2)"
r=$(stat 9 | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(p.get("relay_port"), p.get("relay_rx_threads"))' 2>/dev/null)
[ "$r" = "0 0" ] && ok "and the node advertises no relay port (relay_port 0, no receive threads)" || bad "port-taken node reports: $r"
kill -9 "$P9" "$PY" 2>/dev/null; P9= PY=

# ---- a fleet: two nodes with the plane, one without --------------------------
node 1 2; node 2 2; node 3 0
start 1 && start 2 && start 3 || { echo "relayrxtest: $pass passed, $((fail+1)) failed"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	s=""; for n in 1 2 3; do s="$s$(stat $n | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"], end="|")' 2>/dev/null)"; done
	[ "$s" = "ready 2|ready 2|ready 2|" ] && break; sleep 0.2; i=$((i+1)); done
[ "$s" = "ready 2|ready 2|ready 2|" ] && ok "three nodes formed a fleet" || { bad "fleet never formed: $s"; echo "relayrxtest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 120 python3 - <<'PY'
import json, os, socket, threading, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n")
def get(n, path): return json.load(urllib.request.urlopen("http://127.0.0.1:1856%d%s" % (n, path), timeout=5))
def members(n): return {m["addr"]: m for m in get(n, "/members")["members"]}
def ps(n): return get(n, "/stats")["pubsub"]
def wait(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        if pred(): return True
        time.sleep(0.2)
    return False
def direct(frm, to_addr):
    m = members(frm).get(to_addr, {})
    return m.get("relay", {}).get("direct") is True
ok1 = wait(lambda: direct(1, "127.0.15.2") and direct(2, "127.0.15.1") and direct(3, "127.0.15.1") and direct(3, "127.0.15.2"), 15)
check(ok1, "within seconds each node relays straight to every peer that advertised a relay port and answered its probe")
m1 = members(1)
check(m1["127.0.15.2"]["relay"]["port"] == 17154 and m1["127.0.15.3"]["relay"]["port"] == 0 and m1["127.0.15.3"]["relay"]["direct"] is False,
      "a peer without the plane advertises port 0 and is never taken as direct (%r)" % m1["127.0.15.3"].get("relay"))
p1, p3 = ps(1), ps(3)
check(p1["relay_rx_threads"] == 2 and p1["relay_port"] == 17154 and p3["relay_rx_threads"] == 0 and p3["relay_port"] == 0,
      "/stats: relay_rx_threads 2 on port 17154 with the plane, 0 without")

class R:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10); self.f = self.s.makefile("rb")
    def cmd(self, *a):
        self.s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
        return self.f.readline()
subs = [R(17570 + n) for n in (1, 2, 3)]
for s in subs: s.cmd("SUBSCRIBE", "rx")
time.sleep(0.5)
b = {n: ps(n) for n in (1, 2, 3)}
PER, THREADS = 1500, 8
def burst():
    c = R(17572)
    frame = ("*3\r\n$7\r\nPUBLISH\r\n$2\r\nrx\r\n$4\r\nmsg!\r\n" * PER).encode()
    c.s.sendall(frame)
    for _ in range(PER): c.f.readline()
th = [threading.Thread(target=burst) for _ in range(THREADS)]
[t.start() for t in th]; [t.join() for t in th]
total = PER * THREADS
wait(lambda: ps(1)["relay_recv"] - b[1]["relay_recv"] >= total and ps(3)["relay_recv"] - b[3]["relay_recv"] >= total, 15)
a = {n: ps(n) for n in (1, 2, 3)}
d = lambda n, k: a[n][k] - b[n][k]
check(d(1, "relay_recv") == total and d(1, "delivered") == total and d(3, "relay_recv") == total and d(3, "delivered") == total,
      "8 concurrent publishers on node 2: all %d reach node 1 and node 3 (%d, %d)" % (total, d(1, "delivered"), d(3, "delivered")))
check(d(2, "relay_sent_direct") == total and d(2, "relay_sent_cluster") == total,
      "node 2 sent node 1's copies to its relay port and node 3's to its cluster socket (%d direct, %d cluster)" % (d(2, "relay_sent_direct"), d(2, "relay_sent_cluster")))
check(d(1, "relay_rx_datagrams") >= total and d(3, "relay_rx_datagrams") == 0,
      "node 1's receive threads read them; node 3 has none (%d, %d)" % (d(1, "relay_rx_datagrams"), d(3, "relay_rx_datagrams")))
check(d(1, "relay_lost") == 0 and d(1, "relay_duplicates") == 0 and d(3, "relay_lost") == 0 and d(3, "relay_duplicates") == 0,
      "nothing lost, nothing duplicated, on either path")
m = urllib.request.urlopen("http://127.0.0.1:18561/metrics", timeout=5).read().decode()
check("perfcached_pubsub_relay_rx_threads 2" in m and 'perfcached_pubsub_relay_sent_path_total{path="direct"}' in m,
      "/metrics carries the receive threads and relays by path")
# node 2's own subscriber is served through a queue too, and nothing above
# waited for it
drained = wait(lambda: all(ps(n)["queue_bytes"] == 0 for n in (1, 2, 3)), 5)
q = {n: ps(n) for n in (1, 2, 3)}
check(drained and all(q[n]["queue_dropped"] == b[n]["queue_dropped"] for n in (1, 2, 3)),
      "no worker queue reached its cap, and every queue drained (dropped %s, bytes %s)" % (
      [q[n]["queue_dropped"] - b[n]["queue_dropped"] for n in (1, 2, 3)], [q[n]["queue_bytes"] for n in (1, 2, 3)]))
check("perfcached_pubsub_queue_dropped_total 0" in m and "perfcached_pubsub_queue_bytes 0" in m,
      "/metrics carries the queue drops and bytes")
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
grep -q "pub/sub relays on 127.0.15.1:17154, 2 receive thread(s)" "$D/n1.log" && ok "the node logs its relay port and threads at start" || bad "no start line in node 1's log"
[ "$(grep -c 'now use its relay port' "$D/n2.log")" = 1 ] && ok "node 2 logged the switch to node 1's relay port once" || bad "switch lines on node 2: $(grep -c 'now use its relay port' "$D/n2.log")"
grep -q "stopped answering\|has not answered" "$D/n1.log" "$D/n2.log" "$D/n3.log" && bad "a fallback or silence warning appeared on a healthy fleet" || ok "no fallback warnings on a healthy fleet"

# ---- node 1 comes back without the plane: a configuration, not an outage ----
kill "$P1"; i=0; while kill -0 "$P1" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
node 1 0; mv "$D/n1.log" "$D/n1a.log"
start 1 || bad "node 1 did not restart"
i=0; while [ $i -lt 75 ]; do
	r=$(curl -s -m 3 http://127.0.0.1:18562/members | python3 -c 'import json,sys
m=[x for x in json.load(sys.stdin)["members"] if x["addr"]=="127.0.15.1"]
r = m[0].get("relay", {}) if m else {}
print(r.get("port"), r.get("direct"))' 2>/dev/null)
	[ "$r" = "0 False" ] && break; sleep 0.2; i=$((i+1)); done
[ "$r" = "0 False" ] && ok "node 2 sees node 1's relay port withdrawn and stops relaying to it directly" || bad "node 2's view of node 1 after the restart: $r"
sleep 2.5   # the path's once-a-second tick is where a fallback would be logged
[ "$(grep -c 'no longer has a pub/sub relay port' "$D/n2.log")" = 1 ] && ok "node 2 logged the withdrawal once, as a notice" || bad "withdrawal lines on node 2: $(grep -c 'no longer has a pub/sub relay port' "$D/n2.log")"
grep -q "stopped answering" "$D/n2.log" "$D/n3.log" && bad "a withdrawn port was reported as an outage: $(grep -h 'stopped answering' "$D/n2.log" "$D/n3.log" | head -1)" || ok "and not as a peer that stopped answering"
echo "relayrxtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
