#!/bin/sh
# fleetresettest.sh - S358: the page's reset-stats resets the FLEET.  The
# page shows fleet figures first, summed from every member; a reset that
# reached only the node it was pressed on left the other members' totals
# standing under it.  POST /reset-stats now resets this node and sends
# M_STATS_RESET to every live peer, each counting from the SENDER's moment.
# RESP's CONFIG RESETSTAT stays the node's own, as redis-cli expects.
#   - three nodes, eager; misses on every node
#   - POST /reset-stats on node 1: all three report node 1's reset time
#     and count their misses from 0
#   - CONFIG RESETSTAT on node 2 a second later: node 2 alone moves on
# Fail-first: on 0.5.6.3 nodes 2 and 3 are never reset.
# Usage: test/fleetresettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfleetreset.XXXXXX); P1= P2= P3=
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
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
client = fr-client-secret
cluster = fr-cluster-secret
[listen]
tcp = 127.0.0.1:1946$1
resp = 127.0.0.1:1947$1
http = 127.0.0.1:1948$1
plaintext = loopback
[cluster]
multicast = 239.255.77.61:19458
advertise = 127.0.47.$1
mode = eager
[collection 0]
buckets_log2 = 10
CONF
	chmod 640 "$D/n$1.conf"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; return 1; }
peers() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1948%s/stats" % sys.argv[1], timeout=4))
    print("%s %s" % (d.get("state"), d["cluster"].get("peers_up")))
except Exception:
    print("? ?")' "$1" 2>/dev/null; }
node 1; node 2; node 3; start 1 && start 2 && start 3 || { bad "a node did not start"; echo "fleetresettest: $pass passed, $fail failed"; exit 1; }
i=0; while [ $i -lt 150 ]; do
	set -- $(peers 1); a="$1 $2"; set -- $(peers 2); b="$1 $2"; set -- $(peers 3); c="$1 $2"
	[ "$a" = "ready 2" ] && [ "$b" = "ready 2" ] && [ "$c" = "ready 2" ] && break
	sleep 0.2; i=$((i+1))
done
[ "$a" = "ready 2" ] && [ "$b" = "ready 2" ] && [ "$c" = "ready 2" ] && ok "three nodes formed a fleet" \
	|| { bad "fleet never formed: $a / $b / $c"; echo "fleetresettest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" timeout 60 python3 - <<'PY' 2> "$D/py.err"
import json, os, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
def stats(n): return json.loads(urllib.request.urlopen("http://127.0.0.1:1948%d/stats" % n, timeout=5).read())
def col(st): return [c for c in st["collections"] if c.get("name") == "0"][0]
def resp(n, *a):
    s = socket.create_connection(("127.0.0.1", 19470 + n), 5); f = s.makefile("rb")
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    h = f.readline()
    if h[:1] == b"$" and int(h[1:]) >= 0: f.read(int(h[1:]) + 2)
    s.close(); return h
for n in (1, 2, 3):
    for k in range(3):
        resp(n, b"GET", b"fr-absent-%d-%d" % (n, k))
time.sleep(0.5)
before = {n: col(stats(n)).get("misses", 0) for n in (1, 2, 3)}
check(all(v > 0 for v in before.values()), "every node counted misses (%s)" % before)
r = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:19481/reset-stats", data=b"", method="POST"), timeout=5).read())
at = r.get("at")
deadline = time.time() + 5
while time.time() < deadline:
    ra = {n: stats(n)["since"].get("reset_at") for n in (1, 2, 3)}
    if all(v == at for v in ra.values()):
        break
    time.sleep(0.2)
check(all(v == at for v in ra.values()), "POST /reset-stats on node 1 reached the fleet: every node counts from %s (%s)" % (at, ra))
after = {n: col(stats(n)).get("misses") for n in (1, 2, 3)}
check(all(v == 0 for v in after.values()), "every node's misses start again (%s)" % after)
time.sleep(1.2)                        # a reset in the next second
h = resp(2, b"CONFIG", b"RESETSTAT")
time.sleep(0.5)
ra2 = {n: stats(n)["since"].get("reset_at") for n in (1, 2, 3)}
check(h.startswith(b"+OK") and ra2[2] > at and ra2[1] == at and ra2[3] == at,
      "CONFIG RESETSTAT on node 2 resets node 2 alone (%s)" % ra2)
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
grep -q "statistics reset by node" "$D/n2.log" && grep -q "statistics reset by node" "$D/n3.log" \
	&& ok "nodes 2 and 3 log whose reset they took" || bad "no 'statistics reset by node' line on node 2 or 3"
echo "fleetresettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
