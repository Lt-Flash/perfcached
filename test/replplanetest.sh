#!/bin/sh
# replplanetest.sh - S461: eager replication on the relay port's threads; a peer without the
# plane keeps the cluster socket; a set/delete storm converges.  Fail-first: v0.7.3.
# Usage: test/replplanetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcreplplane.XXXXXX); P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n> <rx threads>
	mkdir -p "$D/s$1"; cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 4
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = replplane-client-secret
cluster = replplane-cluster-secret
[listen]
tcp = 127.0.0.1:1968$1
resp = 127.0.0.1:1969$1
http = 127.0.0.1:1977$1
plaintext = loopback
[cluster]
multicast = 239.255.77.183:17473
advertise = 127.0.83.$1
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
stat() { curl -s -m 3 "http://127.0.0.1:1977$1/stats"; }

# two nodes with the plane, one without
node 1 2; node 2 2; node 3 0
start 1 && start 2 && start 3 || { echo "replplanetest: $pass passed, $((fail+1)) failed"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	s=""; for n in 1 2 3; do s="$s$(stat $n | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"], end="|")' 2>/dev/null)"; done
	[ "$s" = "ready 2|ready 2|ready 2|" ] && break; sleep 0.2; i=$((i+1)); done
[ "$s" = "ready 2|ready 2|ready 2|" ] && ok "three nodes formed a fleet" || { bad "fleet never formed: $s"; echo "replplanetest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 150 python3 - <<'PY'
import json, os, random, socket, threading, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n")
def cl(n): return json.load(urllib.request.urlopen("http://127.0.0.1:1977%d/stats" % n, timeout=5))["cluster"]
def wait(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        if pred(): return True
        time.sleep(0.2)
    return False
class R:
    def __init__(self, n):
        self.s = socket.create_connection(("127.0.0.1", 19690 + n), timeout=10); self.f = self.s.makefile("rb")
    def send(self, *a):
        self.s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode())
    def reply(self):
        l = self.f.readline()
        if l.startswith(b"$"):
            n = int(l[1:])
            if n < 0: return None
            v = self.f.read(n + 2)[:-2]
            return v.decode()
        return l.strip().decode()
    def cmd(self, *a):
        self.send(*a); return self.reply()
    def gets(self, keys):
        for k in keys: self.send("GET", k)
        return [self.reply() for _ in keys]

# node 3 has no relay port, but sends to both of the others' (a sender needs none)
ok1 = wait(lambda: [cl(n).get("repl_plane_peers_direct") for n in (1, 2, 3)] == [1, 1, 2], 15)
check(ok1, "nodes 1 and 2 replicate to each other's relay port and node 3 to both; nobody to node 3's (%r)"
      % [cl(n).get("repl_plane_peers_direct") for n in (1, 2, 3)])
b = {n: cl(n) for n in (1, 2, 3)}

N, CONNS = 4000, 8
def writer(c):
    r = R(1)
    ks = ["rp-%d-%d" % (c, i) for i in range(N // CONNS)]
    for k in ks: r.send("SET", k, "v" + k)
    for _ in ks: r.reply()
th = [threading.Thread(target=writer, args=(c,)) for c in range(CONNS)]
[t.start() for t in th]; [t.join() for t in th]
keys = ["rp-%d-%d" % (c, i) for c in range(CONNS) for i in range(N // CONNS)]
def all_there(n):
    got = R(n).gets(keys)
    return all(g == "v" + k for g, k in zip(got, keys))
check(wait(lambda: all_there(2) and all_there(3), 15), "%d sets from 8 connections on node 1 reach node 2 and node 3" % N)
a = {n: cl(n) for n in (1, 2, 3)}
d = lambda n, k: a[n].get(k, 0) - b[n].get(k, 0)
# S469: a group over max_datagram (1,232) goes to the relay port by TCP, a smaller one by UDP
check(d(1, "repl_plane_sent_direct") + d(1, "repl_plane_tcp_sent") > 0 and d(1, "repl_plane_sent_cluster") > 0,
      "node 1 sent node 2's groups to its relay port and node 3's to its cluster socket (%d direct, %d by TCP, %d cluster)" % (d(1, "repl_plane_sent_direct"), d(1, "repl_plane_tcp_sent"), d(1, "repl_plane_sent_cluster")))
check(d(2, "repl_plane_rx_groups") > 0 and d(3, "repl_plane_rx_groups") == 0,
      "node 2's relay threads applied them; node 3 has none (%d, %d)" % (d(2, "repl_plane_rx_groups"), d(3, "repl_plane_rx_groups")))
check(all(a[n].get("repl_plane_rx_refused", 0) == 0 for n in (1, 2, 3)), "no group refused at a relay port")

# the storm: sets on the relay threads, deletes as tombstones on the cluster thread
K = ["storm-%d" % i for i in range(200)]
stop = time.time() + 4
def storm(seed):
    rnd = random.Random(seed); r = R(1); out = 0
    while time.time() < stop:
        k = rnd.choice(K)
        if rnd.random() < 0.5: r.send("SET", k, "s%d-%d" % (seed, out))
        else: r.send("DEL", k)
        out += 1
        if out % 64 == 0:
            for _ in range(64): r.reply()
    for _ in range(out % 64): r.reply()
th = [threading.Thread(target=storm, args=(s,)) for s in range(CONNS)]
[t.start() for t in th]; [t.join() for t in th]
truth = R(1).gets(K)
same = lambda: R(2).gets(K) == truth and R(3).gets(K) == truth
conv = wait(same, 15)
diff = [(k, t, x, y) for k, t, x, y in zip(K, truth, R(2).gets(K), R(3).gets(K)) if not (t == x == y)]
check(conv, "after a set/delete storm from 8 connections every key reads the same on all three nodes "
      "(%d differ, first %r; %d present on node 1)" % (len(diff), diff[:2], sum(t is not None for t in truth)))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
# node ids are the master's to hand out: count the lines, not their numbers
[ "$(grep -c 'replication to node [0-9]* now uses its relay port' "$D/n1.log")" = 1 ] && [ "$(grep -c 'replication to node [0-9]* now uses its relay port' "$D/n2.log")" = 1 ] \
	&& ok "nodes 1 and 2 each logged one switch to a relay port (the other's), none for node 3" \
	|| bad "switch lines on nodes 1, 2: $(grep -c 'now uses its relay port' "$D/n1.log"), $(grep -c 'now uses its relay port' "$D/n2.log")"
grep -q "answering replication probes\|but not replication" "$D/n1.log" "$D/n2.log" "$D/n3.log" && bad "a replication fallback line on a healthy fleet: $(grep -h 'answering replication probes\|but not replication' "$D/n1.log" "$D/n2.log" "$D/n3.log" | head -1)" || ok "no replication fallback lines on a healthy fleet"
echo "replplanetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
