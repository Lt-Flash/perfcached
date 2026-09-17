#!/bin/sh
# colsfleettest.sh - S163: the collection figures ride the membership
# plane.  On a three-node store-mode fleet every member's per-collection
# block (its OWN client hits and misses, entries, stores, removes,
# expired, named by hash) appears in /members on every node; the fleet
# total of client hits equals the sum of the members' own /stats
# hits_client; a pull one node serves for a peer moves the server's
# client figures not at all; the map's hash joins to /stats' `hash`;
# a standalone daemon publishes no members, so the page has no fleet
# column to show.
# Fail-first: v0.4.0-rc1's ALIVE carries no collection block and its
# /stats rows carry no hash.
# Usage: test/colsfleettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccols.XXXXXX); P1= P2= P3= P4=
trap 'for v in "$P1" "$P2" "$P3" "$P4"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = cols-client-secret
cluster = cols-cluster-secret
[listen]
tcp = 127.0.0.1:1750$1
http = 127.0.0.1:1850$1
plaintext = loopback
[cluster]
multicast = 239.255.77.50:17150
advertise = 127.0.13.$1
mode = store
collections = b
[collection b]
buckets_log2 = 12
CONF
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; cat "$D/n$1.log" | tail -5; return 1; }
state() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1850%s/stats" % sys.argv[1], timeout=4))
    print("%s %s" % (d.get("state"), d["cluster"].get("peers_up")))
except Exception:
    print("? ?")' "$1" 2>/dev/null; }
node 1; node 2; node 3
cat > "$D/n4.conf" <<CONF
[daemon]
workers = 1
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = cols-client-secret
cluster = cols-cluster-secret
[listen]
tcp = 127.0.0.1:17504
http = 127.0.0.1:18504
plaintext = loopback
[collection b]
buckets_log2 = 12
CONF
chmod 600 "$D"/n?.conf
start 1 && start 2 && start 3 && start 4 || exit 1
# every node must see the other two: a read through node 2 pulls from node 1
# only once node 2 counts node 1 live, and a miss there is remembered
all_formed() { for n in 1 2 3; do set -- $(state $n); [ "$1" = ready ] && [ "$2" = 2 ] || return 1; done; }
i=0; while [ $i -lt 150 ]; do all_formed && break; sleep 0.2; i=$((i+1)); done
all_formed && ok "three nodes formed a store-mode fleet, each seeing the other two" || { bad "fleet never formed: $(state 1) / $(state 2) / $(state 3)"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n")
def get(port, path):
    try: return json.load(urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=5))
    except Exception: return {}
def col(port):
    for c in get(port, "/stats").get("collections", []):
        if c.get("name") == "b": return c
    return {}
def members(port): return {m.get("node"): m for m in get(port, "/members").get("members", [])}
def fleet_b(port):
    out = {}
    for n, m in members(port).items():
        for c in m.get("collections") or []:
            out[n] = c
    return out
def jcall(port, method, params):
    s = socket.create_connection(("127.0.0.1", port), timeout=5); f = s.makefile("rwb")
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}) + "\n").encode()); f.flush()
    r = json.loads(f.readline()); f.close(); s.close(); return r
def wait(pred, secs):
    for _ in range(int(secs * 5)):
        if pred(): return True
        time.sleep(0.2)
    return False
H1, H2, H3, H4 = 18501, 18502, 18503, 18504
T1, T2, T3 = 17501, 17502, 17503
jcall(T1, "set", {"col": "b", "key": "k1", "value": "v"})
for _ in range(3): jcall(T1, "get", {"col": "b", "key": "k1"})
c1 = col(H1)
check(c1.get("hits_client") == 3 and len(c1.get("hash", "")) == 16, "node 1: three client hits on b, and the row carries a 16-hex hash (%r, %r)" % (c1.get("hits_client"), c1.get("hash")))
r2 = [jcall(T2, "get", {"col": "b", "key": "k1"}) for _ in range(2)]
c2 = get(H2, "/stats").get("cluster", {})
check(all(r.get("result", {}).get("found") is True for r in r2),
      "node 2 answers k1 through a pull from node 1 (found %s; node 2 peers_up %s, pulls sent %s hits %s misses %s timeouts %s, negative-cache hits %s)" % (
      [r.get("result", {}).get("found") for r in r2], c2.get("peers_up"), c2.get("pull_sent"), c2.get("pull_hits"),
      c2.get("pull_misses"), c2.get("pull_timeouts"), c2.get("neg_hits")))
for _ in range(4): jcall(T3, "get", {"col": "b", "key": "nope"})
check(col(H1).get("hits_client") == 3, "serving node 2's pulls moved node 1's CLIENT hits not at all (%r)" % col(H1).get("hits_client"))
check(col(H3).get("misses_client") == 4, "node 3: four client misses (%r)" % col(H3).get("misses_client"))
own = {n: col(h) for n, h in ((1, H1), (2, H2), (3, H3))}
want_hits = sum(c.get("hits_client", 0) for c in own.values())
check(wait(lambda: len(fleet_b(H3)) == 3 and sum(c.get("hits", 0) for c in fleet_b(H3).values()) == want_hits, 6),
      "node 3's map carries every member's block for b and their client hits sum to the members' own (%r)" % want_hits)
fb = fleet_b(H3)
mm = members(H3)
check(all(m.get("collections_total") == 1 for m in mm.values()), "collections_total is 1 on every member")
check(all(c.get("hash") == own[1].get("hash") for c in fb.values()) and own[1].get("hash") == own[2].get("hash") == own[3].get("hash"),
      "the block's hash equals the /stats hash on every node - the join key")
byhits = sorted(c.get("hits") for c in fb.values())
check(byhits == sorted(c.get("hits_client") for c in own.values()), "each member's share is what it served itself (%r)" % byhits)
check(sum(c.get("entries", 0) for c in fb.values()) == sum(c.get("entries", 0) for c in own.values()),
      "fleet entries are the copies the members hold, summed (%r)" % sum(c.get("entries", 0) for c in fb.values()))
check(sum(c.get("misses", 0) for c in fb.values()) == sum(c.get("misses_client", 0) for c in own.values()),
      "fleet misses equal the members' own client misses")
check(set(fb[list(fb)[0]].keys()) == {"hash", "entries", "hits", "misses", "stores", "removes", "expired"}, "seven fields a collection")
check(wait(lambda: len(fleet_b(H1)) == 3 and len(fleet_b(H2)) == 3, 6), "nodes 1 and 2 carry the same blocks")
sa = get(H4, "/members")
check(sa.get("members") == [], "a standalone daemon publishes no members: no fleet column")
check(len(col(H4).get("hash", "")) == 16, "while its own /stats rows still carry the hash")
page = urllib.request.urlopen("http://127.0.0.1:%d/" % H1, timeout=5).read().decode("utf-8", "replace")
check("fleet totals first" in page and "function fv(" in page and "copies across" in page, "the page renders fleet totals first with this node beside")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "colsfleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
