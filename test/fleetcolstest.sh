#!/bin/sh
# fleetcolstest.sh - S164: the daemon answers for the fleet per collection.
# On a three-node EAGER fleet every member holds every entry, so /stats'
# `fleet` block must report the entries once (the fullest member), not the
# copies summed; client writes, hits and removes are counted once, on the
# node the client reached, even though every member applies each write;
# an expiry each member counts on its own copy is reported once; the block
# is the same on every node; a standalone daemon has none.
# Fail-first: v0.4.0-rc4 has no `fleet` block, and its gossiped `stores`
# counted every apply (8,663 on 245 for ~2,992 client writes).
# Usage: test/fleetcolstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfleetcols.XXXXXX); P1= P2= P3= P4=
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
client = fleetcols-client-secret
cluster = fleetcols-cluster-secret
[listen]
tcp = 127.0.0.1:1754$1
http = 127.0.0.1:1854$1
plaintext = loopback
[cluster]
multicast = 239.255.77.58:17158
advertise = 127.0.18.$1
mode = eager
collections = f
[collection f]
buckets_log2 = 12
CONF
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; tail -5 "$D/n$1.log"; return 1; }
state() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1854%s/stats" % sys.argv[1], timeout=4))
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
client = fleetcols-client-secret
cluster = fleetcols-cluster-secret
[listen]
tcp = 127.0.0.1:17544
http = 127.0.0.1:18544
plaintext = loopback
[collection f]
buckets_log2 = 12
CONF
chmod 600 "$D"/n?.conf
start 1 && start 2 && start 3 && start 4 || exit 1
all_formed() { for n in 1 2 3; do set -- $(state $n); [ "$1" = ready ] && [ "$2" = 2 ] || return 1; done; }
i=0; while [ $i -lt 150 ]; do all_formed && break; sleep 0.2; i=$((i+1)); done
all_formed && ok "three nodes formed an eager fleet, each seeing the other two" || { bad "fleet never formed: $(state 1) / $(state 2) / $(state 3)"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n"); res.flush()
def get(port, path):
    try: return json.load(urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=5))
    except Exception: return {}
def col(port):
    for c in get(port, "/stats").get("collections", []):
        if c.get("name") == "f": return c
    return {}
def fl(port): return col(port).get("fleet") or {}
# a native JSON connection also carries unsolicited notifications (a member
# joining sends {"notify":"membership",...} to every client), so the reply
# is the first line that answers this request's id - not the first line
def jcall(port, method, params):
    s = socket.create_connection(("127.0.0.1", port), timeout=5); f = s.makefile("rwb")
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}) + "\n").encode()); f.flush()
    r = {}
    for _ in range(64):
        line = f.readline()
        if not line: break
        r = json.loads(line)
        if "id" in r: break
    f.close(); s.close(); return r
def wait(pred, secs):
    for _ in range(int(secs * 5)):
        if pred(): return True
        time.sleep(0.2)
    return False
H = {1: 18541, 2: 18542, 3: 18543}
T = {1: 17541, 2: 17542, 3: 17543}

# 5 client writes through node 1, 3 through node 2; eager applies all 8 on every member
for k in range(5): jcall(T[1], "set", {"col": "f", "key": "a%d" % k, "value": "v"})
for k in range(3): jcall(T[2], "set", {"col": "f", "key": "b%d" % k, "value": "v"})
check(wait(lambda: all(col(H[n]).get("entries") == 8 for n in H), 10),
      "every member holds all 8 entries (%r)" % [col(H[n]).get("entries") for n in H])
own = {n: col(H[n]) for n in H}
# the positive control: the per-node all-origin counters DO count every copy,
# so a fleet figure made by summing them is reachable and would be wrong
check(sum(c.get("stores", 0) for c in own.values()) >= 24,
      "summed, the members' own stores count every apply (%r >= 24)" % sum(c.get("stores", 0) for c in own.values()))
check([own[n].get("stores_client") for n in (1, 2, 3)] == [5, 3, 0],
      "stores_client is the writes clients made on each node (%r)" % [own[n].get("stores_client") for n in (1, 2, 3)])
def fleet_is(n, key, want): return fl(H[n]).get(key) == want
check(wait(lambda: all(fleet_is(n, "reporting", 3) and fleet_is(n, "entries", 8) for n in H), 8),
      "every node's fleet entries are 8, the entries - not 24, the copies (%r)" % [fl(H[n]).get("entries") for n in H])
check(wait(lambda: fleet_is(1, "copies", 24), 8) and fl(H[1]).get("basis") == "fullest" and fl(H[1]).get("members") == 3,
      "eager: basis fullest, 3 members, 24 copies behind the 8 (%r)" % {k: fl(H[1]).get(k) for k in ("basis", "members", "copies")})
check(wait(lambda: all(fleet_is(n, "stores", 8) for n in H), 8),
      "fleet stores are the 8 client writes, each counted once (%r)" % [fl(H[n]).get("stores") for n in H])

# client lookups and a removal, each on one node
h3 = col(H[3]).get("hits_client", 0)
for _ in range(4): jcall(T[3], "get", {"col": "f", "key": "a0"})
jcall(T[2], "del", {"col": "f", "key": "b0"})
check(col(H[3]).get("hits_client", 0) - h3 == 4, "node 3 counted its 4 client hits (%r)" % (col(H[3]).get("hits_client", 0) - h3))
def hsum(): return sum(col(H[n]).get("hits_client", 0) for n in H)
check(wait(lambda: all(fleet_is(n, "hits", hsum()) for n in H), 8),
      "fleet hits are the members' client hits summed (%r vs %r)" % ([fl(H[n]).get("hits") for n in H], hsum()))
check(wait(lambda: all(fleet_is(n, "removes", 1) and fleet_is(n, "entries", 7) for n in H), 8),
      "one client delete: fleet removes 1 and entries 7 on every node (%r, %r)" % (
      [fl(H[n]).get("removes") for n in H], [fl(H[n]).get("entries") for n in H]))

# expiry: every member's sweep counts its own copy
for k in range(3): jcall(T[1], "set", {"col": "f", "key": "t%d" % k, "value": "v", "ttl": 1})
check(wait(lambda: all(col(H[n]).get("expired", 0) >= 3 for n in H), 25),
      "each member expired its own 3 copies (%r)" % [col(H[n]).get("expired") for n in H])
xs = [col(H[n]).get("expired", 0) for n in H]
check(wait(lambda: all(fleet_is(n, "expired", max(col(H[m]).get("expired", 0) for m in H)) for n in H), 8),
      "fleet expired is the fullest member's %r, not the %r copies (%r)" % (max(xs), sum(xs), [fl(H[n]).get("expired") for n in H]))

sa = col(18544)
check(sa.get("name") == "f" and "fleet" not in sa, "a standalone daemon's row has no fleet block")
page = urllib.request.urlopen("http://127.0.0.1:%d/" % H[1], timeout=5).read().decode("utf-8", "replace")
check("x.fleet&&x.fleet.members>1" in page and "fv(FG.entries,x.entries,LB)" in page and "fv(FG.stores,x.stores_client)" in page,
      "the page renders the daemon's fleet block, not its own sum")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "fleetcolstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
