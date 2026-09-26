#!/bin/sh
# unknownfleettest.sh - S165: the unknown-command list is the FLEET's.
# Three eager members.  An unknown JSON method sent twice to node 1 and
# once to node 2 appears on node 3's /stats as one row counted 3, with
# node 3's own share 0 and each sender's share on its own page; every
# node reports every member; the row names the member that saw it last.
# Node 2 then dies: its share leaves node 3's list with it, and the
# member count falls to 2.
# Fail-first: v0.4.0-rc5 has no `unknown_commands` in /stats, and a build
# without the M_UNKCMD exchange shows node 3 a list of its own only.
# Usage: test/unknownfleettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcunkfleet.XXXXXX); P3= P4= P5=
trap 'for v in "$P3" "$P4" "$P5"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# nodes 3, 4, 5 - their ports end in their number, clear of unknowntest's 1..2
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
client = unkfleet-client-secret
cluster = unkfleet-cluster-secret
[listen]
tcp = 127.0.0.1:1779$1
http = 127.0.0.1:1879$1
plaintext = loopback
[cluster]
multicast = 239.255.77.59:17159
advertise = 127.0.19.$1
mode = eager
collections = f
[collection f]
buckets_log2 = 10
CONF
	chmod 600 "$D/n$1.conf"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & eval "P$1=\$!"
	i=0; while [ $i -lt 120 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  node $1 never became ready"; tail -5 "$D/n$1.log"; return 1; }
state() { timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:1879%s/stats" % sys.argv[1], timeout=4))
    print("%s %s" % (d.get("state"), d["cluster"].get("peers_up")))
except Exception:
    print("? ?")' "$1" 2>/dev/null; }
node 3; node 4; node 5
start 3 && start 4 && start 5 || exit 1
all_formed() { for n in 3 4 5; do set -- $(state $n); [ "$1" = ready ] && [ "$2" = 2 ] || return 1; done; }
i=0; while [ $i -lt 150 ]; do all_formed && break; sleep 0.2; i=$((i+1)); done
all_formed && ok "three nodes formed an eager fleet" || { bad "fleet never formed: $(state 3) / $(state 4) / $(state 5)"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" P4="$P4" python3 - <<'PY'
import json, os, signal, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(cond, name): res.write(("P " if cond else "F ") + name + "\n"); res.flush()
H = {3: 18793, 4: 18794, 5: 18795}
T = {3: 17793, 4: 17794, 5: 17795}
def stats(n):
    try: return json.load(urllib.request.urlopen("http://127.0.0.1:%d/stats" % H[n], timeout=5))
    except Exception: return {}
def unk(n): return stats(n).get("unknown_commands") or {}
def node_id(n): return stats(n).get("cluster", {}).get("node")
def row(n, name="frobnicate"):
    for r in unk(n).get("rows", []):
        if r.get("name") == name and r.get("dialect") == "json": return r
    return {}
def jcall(port, method):
    s = socket.create_connection(("127.0.0.1", port), timeout=5); f = s.makefile("rwb")
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": {}}) + "\n").encode()); f.flush()
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

ids = {n: node_id(n) for n in H}
check(wait(lambda: all(unk(n).get("members") == 3 and unk(n).get("reporting") == 3 for n in H), 10),
      "every node counts 3 members and hears all 3 tables, even empty ones (%r)" %
      [(unk(n).get("members"), unk(n).get("reporting")) for n in H])
jcall(T[3], "frobnicate"); jcall(T[3], "frobnicate")
time.sleep(1.2)                          # node 4's sighting is the LATER one
jcall(T[4], "frobnicate")
check(wait(lambda: all(row(n).get("count") == 3 for n in H), 10),
      "the row is counted 3 on EVERY node (%r)" % [row(n).get("count") for n in H])
check(row(3).get("here") == 2 and row(4).get("here") == 1 and row(5).get("here") == 0,
      "each page's `here` is its own share: 2 / 1 / 0 (%r)" % [row(n).get("here") for n in H])
check(row(5).get("node") == ids[4], "node 5's row names the member that saw it last: node 4's id %r (%r)" % (ids[4], row(5).get("node")))
check(unk(5).get("frames_received", 0) > 0 and unk(5).get("frames_refused") == 0,
      "node 5 received tables and refused none (%r / %r)" % (unk(5).get("frames_received"), unk(5).get("frames_refused")))

# node 4 dies: its share leaves the fleet list with it
os.kill(int(os.environ["P4"]), signal.SIGKILL)
check(wait(lambda: unk(5).get("members") == 2 and row(5).get("count") == 2, 30),
      "after node 4 dies node 5 shows 2 members and a count of 2 (%r, %r)" % (unk(5).get("members"), row(5).get("count")))
check(row(5).get("here") == 0 and row(3).get("here") == 2, "the survivors' shares are unchanged")
PY
while IFS= read -r l; do
	case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac
done < "$RES"
[ -s "$RES" ] || bad "driver produced no results"
echo "unknownfleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
