#!/bin/sh
# clientsfleettest.sh - S160: the fleet-wide clients figure rides the
# membership plane.  On a three-node fleet every member's open count
# (and its dialect split) appears in /members on EVERY node, at most one
# heartbeat stale, and the sum equals the sum of the members' own
# /stats clients.open; a standalone daemon publishes no members, so the
# page's denominator is absent there rather than "N / N".
# Fail-first: v0.4.0-rc1's ALIVE has no clients field, so /members
# carries no `clients` object.
# Usage: test/clientsfleettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcclf.XXXXXX); P1= P2= P3= P4=
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
client = clf-client-secret
cluster = clf-cluster-secret
[listen]
tcp = 127.0.0.1:1749$1
http = 127.0.0.1:1849$1
plaintext = loopback
[cluster]
multicast = 239.255.77.49:17149
advertise = 127.0.12.$1
mode = spread
replicas = 2
collections = b
[collection b]
buckets_log2 = 12
CONF
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; return 1; }
state() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1849%s/stats" % sys.argv[1], timeout=4))
    print("%s %s" % (d.get("state"), d["cluster"].get("peers_up")))
except Exception:
    print("? ?")' "$1" 2>/dev/null; }
node 1; node 2; node 3; start 1 && start 2 && start 3 || exit 1
i=0; while [ $i -lt 150 ]; do set -- $(state 1); [ "$1" = ready ] && [ "$2" = 2 ] && break; sleep 0.2; i=$((i+1)); done
set -- $(state 1); [ "$1" = ready ] && [ "$2" = 2 ] && ok "three nodes formed a fleet" || { bad "fleet never formed: $(state 1)"; exit 1; }

# a standalone daemon beside it: no [cluster], so no map and no denominator
cat > "$D/n4.conf" <<CONF
[daemon]
workers = 1
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = clf-client-secret
cluster = clf-cluster-secret
[listen]
tcp = 127.0.0.1:17494
http = 127.0.0.1:18494
plaintext = loopback
[collection b]
buckets_log2 = 12
CONF
chmod 600 "$D"/n?.conf
start 4 || exit 1

RES="$D/res"; : > "$RES"
RESFILE="$RES" python3 - <<'PY'
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n")
def get(port, path):
    try: return json.load(urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=5))
    except Exception: return {}
def members(port): return {m.get("node"): m for m in get(port, "/members").get("members", [])}
def counts(port): return {n: (m.get("clients") or {}).get("open") for n, m in members(port).items()}
def opens(): return [get(18490 + n, "/stats").get("clients", {}).get("open", 0) for n in (1, 2, 3)]
def wait_counts(port, want, secs):
    for _ in range(int(secs * 5)):
        if sorted(counts(port).values(), key=lambda v: -1 if v is None else v) == sorted(want): return True
        time.sleep(0.2)
    return False
def jconn(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5); f = s.makefile("rwb")
    f.write(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n'); f.flush(); f.readline()
    return s, f
m = members(18493)
check(len(m) == 3 and all(isinstance(x.get("clients"), dict) for x in m.values()),
      "every member on node 3's map carries a clients object (%d members)" % len(m))
check(all(set(x["clients"].keys()) == {"open", "resp", "binary", "json", "native_resp"} for x in m.values() if x.get("clients")),
      "with the total and the four-way dialect split")
a1, a2 = jconn(17491), jconn(17491)
b1 = jconn(17492)
check(wait_counts(18493, [2, 1, 0], 6), "two JSON clients on node 1 and one on node 2 show on node 3's map within a few heartbeats (%r)" % counts(18493))
mm = members(18493)
n1 = [x for x in mm.values() if x["clients"]["open"] == 2]
check(len(n1) == 1 and n1[0]["clients"]["json"] == 2 and not n1[0].get("self"), "node 1's entry says json=2, and it is not the reader's own row")
check(sum(v for v in counts(18493).values() if v is not None) == sum(opens()) == 3,
      "the map's sum equals the sum of the members' own /stats clients.open (%r vs %r)" % (counts(18493), opens()))
check(wait_counts(18491, [2, 1, 0], 6) and wait_counts(18492, [2, 1, 0], 6), "nodes 1 and 2 see the same figures")
for s, f in (a1, a2, b1): f.close(); s.close()
check(wait_counts(18493, [0, 0, 0], 6), "closed clients leave the map (%r)" % counts(18493))
sa = get(18494, "/members")
check(sa.get("members") == [], "a standalone daemon publishes no members, so the page has no denominator to show")
check(get(18494, "/stats").get("clients", {}).get("open") == 0, "while its own /stats count is still there")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "clientsfleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
