#!/bin/sh
# upnoroomshardtest.sh - a shard read through a non-owner gets the owner's fetched value even when the
# owner's full arena has no room for the copy; fill before any read, or a read leaves a chunk free.
set -u
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=19761
for p in $UP 19771 19772 19773 19774 19775 19776 19779; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "upnoroomshardtest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcupnrsh.XXXXXX)
P1ID= P2ID= FPID=
trap 'for p in $P1ID $P2ID $FPID; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$D/up"
python3 - "$D/up/data.json" <<'P'
import json, sys
d = {"tok:s%d" % i: {"type": "string", "value": "v-s%d" % i} for i in range(1, 21)}
json.dump(d, open(sys.argv[1], "w"))
P
echo '{"password": "nrs-pass-1", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

node() { # node <n>: tcp 1977n, resp 1977(n+2), http 1977(n+4)
	mkdir -p "$D/state$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 16
[secrets]
client = nrs-client-secret
cluster = nrs-cluster-secret
[listen]
tcp = 127.0.0.1:1977$1
resp = 127.0.0.1:1977$(($1 + 2))
resp_collections = 0
http = 127.0.0.1:1977$(($1 + 4))
plaintext = loopback
[wal]
dir = $D/state$1/wal
probe = no
segment_mb = 8
[cluster]
multicast = 239.255.79.71:19779
advertise = 127.0.79.$1
mode = shard
collections = 0
pull_timeout_ms = 500
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_password = nrs-pass-1
upstream_writes = through
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
C
	chmod 600 "$D/n$1.conf"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || {
		bad "member $1 did not start: $(grep -m1 -E 'CRIT|ERR' "$D/n$1.log" | cut -c1-160)"
		echo "upnoroomshardtest: $pass passed, $fail failed"; exit 1; }
}
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:1977%d/stats" % (int(sys.argv[1]) + 4), timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null; }
node 1; node 2
i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']")" != 1 ]; do sleep 0.2; i=$((i+1)); done
for n in 1 2; do i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done; done
[ "$(st 1 "s['cluster']['peers_up']") $(st 1 "s['state']") $(st 2 "s['state']")" = "1 ready ready" ] ||
	bad "the members: peers_up $(st 1 "s['cluster']['peers_up']"), states $(st 1 "s['state']") $(st 2 "s['state']")"

R=$(python3 - <<'P'
import json, socket, urllib.request
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in
        (y if isinstance(y, bytes) else y.encode() for y in a))
class C:
    def __init__(s, port):
        s.k = socket.create_connection(("127.0.0.1", port), 10); s.f = s.k.makefile("rb")
    def rd(s):
        l = s.f.readline(); t, r = l[:1], l[1:-2]
        if t == b":": return int(r)
        if t == b"$":
            n = int(r)
            return None if n < 0 else s.f.read(n + 2)[:-2].decode()
        if t in (b"*", b"%"):
            n = int(r) * (2 if t == b"%" else 1)
            return None if n < 0 else [s.rd() for _ in range(n)]
        if t == b"_": return None
        return (t + r).decode()
    def cmd(s, *a):
        s.k.sendall(enc(*a)); return s.rd()
    def pipe(s, cmds):
        s.k.sendall(b"".join(enc(*c) for c in cmds)); return [s.rd() for _ in cmds]
def stats(n):
    return json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (19774 + n), timeout=5).read())
def ctr(n):
    s = stats(n)
    u = [c for c in s["collections"] if c["name"] == "0"][0]["upstream"]
    return u.get("fill_failed", 0), s["cluster"].get("pull_fetching", 0)
def pulls(n):
    s = stats(n); c = s["cluster"]; m = s.get("memory", {})
    return "member %d %s peers_up %s arena used %s free %s: %s" % (n, s.get("state"), c.get("peers_up"), m.get("arena_used"),
        m.get("arena_free"), " ".join("%s %s" % (x[5:], c.get(x)) for x in
        ("pull_sent", "pull_hits", "pull_misses", "pull_timeouts", "pull_served", "pull_fetching", "pull_fetched")))
pc = C(19773); pc2 = C(19774)
out = {}
# both arenas filled through both members: each size until a batch through
# each is refused whole (every key's owner full, its own writes included)
n, full, errs_by = 0, 0, {}
SIZES = (2048, 1500, 1000, 900, 800, 700, 600, 500, 450, 400, 350, 300, 250, 200,
         160, 120, 100, 80, 64, 48, 32, 24, 16, 8, 4, 1)
for size in SIZES:
    v = b"x" * size
    while n < 600000:
        whole = 0
        for c in (pc, pc2):
            rs = c.pipe([("SET", "pad:%d" % (n + j), v) for j in range(256)])
            n += 256
            errs = [x for x in rs if isinstance(x, str) and x.startswith("-")]
            for e in errs:
                errs_by[e] = errs_by.get(e, 0) + 1
            whole += len(errs) == len(rs)
        if whole == 2:
            full += 1
            break
first_err = ", ".join("%s x%d" % kv for kv in sorted(errs_by.items()))
ff1, _ = ctr(1); ff2, pf2 = ctr(2)
out.update({"pads": n, "full": full, "err": first_err, "before": [pulls(1), pulls(2)]})
out["s"] = [pc.cmd("GET", "tok:s%d" % i) for i in range(1, 21)]
out["mget"] = pc.cmd("MGET", "tok:s1", "tok:s2", "tok:s3", "tok:s4", "tok:s5", "tok:s6", "tok:none")
g1, _ = ctr(1); g2, pg2 = ctr(2)
out["ff1"], out["ff2"], out["pf2"] = g1 - ff1, g2 - ff2, pg2 - pf2
out["after"] = [pulls(1), pulls(2)]
print(json.dumps(out))
P
)
echo "$R" > "$D/result.json"
J() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$D/result.json" "$1" 2>/dev/null; }

[ "$(J 'd["full"]')" = 26 ] && [ "$(J 'd["pf2"] > 0 and d["ff2"] > 0')" = True ] &&
	ok "0. both arenas refused every size ($(J 'd["pads"]') writes, '$(J 'd["err"]')'); member 2 fetched for $(J 'd["pf2"]') pulls, its fills failed $(J 'd["ff2"]') times (member 1's $(J 'd["ff1"]'))" ||
	bad "0. the premise: $(J 'd["full"]') of 26 sizes refused whole after $(J 'd["pads"]') writes ('$(J 'd["err"]')'), member 2 pull_fetching +$(J 'd["pf2"]'), fill_failed +$(J 'd["ff2"]') - no pull below was relayed; after the fill $(J '" / ".join(d["before"])'); after the reads $(J '" / ".join(d["after"])')"
[ "$(J 'd["s"] == ["v-s%d" % i for i in range(1, 21)]')" = True ] &&
	ok "1. 20 strings through member 1: every value, member 2's included" ||
	bad "1. 20 strings through member 1: $(J '[(i + 1, x) for i, x in enumerate(d["s"]) if x != "v-s%d" % (i + 1)]') (want v-s<n>)"
[ "$(J 'd["mget"]')" = "['v-s1', 'v-s2', 'v-s3', 'v-s4', 'v-s5', 'v-s6', None]" ] &&
	ok "2. MGET through member 1 of keys both own: every value, nil for the absent key" ||
	bad "2. MGET through member 1: $(J 'd["mget"]')"
for n in 1 2; do      # the fill's own alert aside
	grep CRIT "$D/n$n.log" | grep -qv "writes refused, the arena is full" &&
		bad "member $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v "writes refused, the arena is full" | head -2 | tr '\n' ' ' | cut -c1-200)"
done
echo "upnoroomshardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
