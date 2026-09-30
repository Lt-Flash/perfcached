#!/bin/sh
# delprobetest.sh - RV-11: a delete that lands on a node which does not
# hold the key looks where the record IS.
#
# A write taken by node A is applied and acknowledged there while its
# push to the peers batches for up to REPL_FLUSH_MS (3 ms).  A delete of
# the same key that a balancer lands on node B inside that window found
# nothing at B, answered "absent" with no tombstone, and the push then
# put the record on every member - a deleted key alive everywhere.  The
# client cannot carry the ordering across: the one that does this here
# is a Redis client behind HAProxy, and RESP has nowhere to put it.  So
# B asks the fleet, the node that HOLDS the record deletes it and
# tombstones under its own clock (which has observed the record's
# version, so the tombstone is newer than every copy, landed or in
# flight), and B answers what really happened.  Spread unicasts its
# best-ranked holder; store has the same hole without the push - the
# record simply stayed on its home node.
#
# The driver holds a connection to EACH node before it starts and sends
# the write to one and the delete to the other with nothing between -
# tens of microseconds on loopback, far inside the window.  Arms, each
# its own fleet, every node asserted at every sample for 2.5 s:
#   eager  - native set@A del@B, RESP SET@A DEL@C, native jset@A jdel@B;
#            a delete of a key nobody has answers false, and the second
#            miss of the same key sends no probe (the negative cache);
#            a delete a second after the write still works (control);
#            no probe was mistaken for a birth race (several holders
#            answering yes is eager's normal state);
#   store  - set@A del@B removes it from A;
#   spread - K=2: deletes via every node answer true, no copy survives.
# Before the fix: every pair answers false and the keys are everywhere.
# Usage: test/delprobetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcdp.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# pairs <dialect> <portA> <portB> <n> <prefix> : write on A, delete on B, nothing
#        between; prints how many deletes answered "deleted"
# absent <n> <prefix> <port>... : 5 samples over 2.5 s, every key on every node;
#        prints the offenders (empty = absent everywhere, every sample)
# one <port> <json> : one JSON-RPC call, prints the result
import json, socket, sys, time

class J:
    def __init__(s, port):
        s.f = socket.create_connection(("127.0.0.1", port), timeout=8).makefile("rwb"); s.n = 0
    def call(s, method, **params):
        s.n += 1
        s.f.write(json.dumps({"jsonrpc": "2.0", "id": s.n, "method": method, "params": params}).encode() + b"\n"); s.f.flush()
        r = json.loads(s.f.readline()); return r.get("result", r.get("error"))

class R:
    def __init__(s, port):
        s.f = socket.create_connection(("127.0.0.1", port), timeout=8).makefile("rwb")
    def cmd(s, *a):
        s.f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()); s.f.flush()
        r = s.f.readline().decode("latin1").strip()
        if r.startswith("$") and not r.startswith("$-1"): s.f.read(int(r[1:]) + 2)
        return r

what = sys.argv[1]
if what == "pairs":
    dialect, pa, pb, n, prefix = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    hit = 0
    if dialect == "resp":
        a, b = R(pa), R(pb)
        for i in range(n):
            k = "%s%d" % (prefix, i)
            a.cmd("SET", k, "v", "EX", "120")
            if b.cmd("DEL", k) == ":1": hit += 1
    else:
        a, b = J(pa), J(pb)
        for i in range(n):
            k = "%s%d" % (prefix, i)
            if dialect == "json":
                a.call("jset", col="0", key=k, path="$", val={"a": 1}, ttl=120)
                r = b.call("jdel", col="0", key=k, path="$")
            else:
                a.call("set", col="0", key=k, value="v", ttl=120)
                r = b.call("del", col="0", key=k)
            if isinstance(r, dict) and r.get("deleted"): hit += 1
    print(hit)
elif what == "absent":
    n, prefix, ports = int(sys.argv[2]), sys.argv[3], [int(x) for x in sys.argv[4:]]
    conns = [J(p) for p in ports]; off = []
    for t in range(1, 6):
        time.sleep(0.5)
        for ci, c in enumerate(conns):
            for i in range(n):
                r = c.call("exists", col="0", key="%s%d" % (prefix, i))
                if not (isinstance(r, dict) and r.get("exists") is False):
                    off.append("n%d@%dms:%s%d" % (ci + 1, t * 500, prefix, i))
    print(" ".join(off[:6]) + (" (+%d more)" % (len(off) - 6) if len(off) > 6 else ""))
elif what == "one":
    print(json.dumps(J(int(sys.argv[2])).call(**json.loads(sys.argv[3]))))
EOF
pairs()  { python3 "$DRV" pairs "$@"; }
absent() { python3 "$DRV" absent "$@"; }
one()    { python3 "$DRV" one "$1" "$2"; }
stat()   { one "$1" '{"method":"stats"}' | python3 -c "import json,sys; print((json.load(sys.stdin).get(\"cluster\") or {}).get(\"$2\", -1))"; }
entries() { one "$1" '{"method":"stats","col":"0"}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }

# node <arm> <n> <cluster-lines>: arm A on ports 182A<n>, 127.0.59.<A><n>, group .20<A>
node() {
	mkdir -p "$D/st$1$2"
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/st$1$2
[memory]
arena_mb = 64
[secrets]
client = dp-client-secret
cluster = dp-cluster-secret
[listen]
tcp = 127.0.0.1:182$1$2
plaintext = loopback
[cluster]
multicast = 239.255.77.20$1:1740$1
advertise = 127.0.59.$1$2
pull_timeout_ms = 300
$3
collections = 0
[collection 0]
buckets_log2 = 10
pull = 1
EOF
}
start() {
	"$BIN" -f "$D/n$1$2.conf" > "$D/n$1$2.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "node state .* -> ready" "$D/n$1$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1$2 never reported ready:"; tail -3 "$D/n$1$2.log"; exit 1
}
stop_arm() {
	for p in $PIDS; do
		grep -qa -- "$D/n$1" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"
	done
	PIDS=""
	sleep 0.5
}

# ---- eager ---------------------------------------------------------
for n in 1 2 3; do node 1 $n "mode = eager"; done
for n in 1 2 3; do start 1 $n; done
sleep 1
H=$(pairs native 18211 18212 20 en); O=$(absent 20 en 18211 18212 18213)
[ "$H" = 20 ] && [ -z "$O" ] && ok "eager: 20 x set@A del@B with nothing between - every delete answered true, the keys absent on all three nodes at every sample" \
	|| bad "eager: set@A del@B - $H of 20 deletes answered true; still present: ${O:-none}"
H=$(pairs resp 18211 18213 20 er); O=$(absent 20 er 18211 18212 18213)
[ "$H" = 20 ] && [ -z "$O" ] && ok "eager/RESP: 20 x SET@A DEL@C - every DEL answered :1, absent everywhere (the client that cannot carry a hint)" \
	|| bad "eager/RESP: SET@A DEL@C - $H of 20 answered :1; still present: ${O:-none}"
H=$(pairs json 18212 18213 10 ej); O=$(absent 10 ej 18211 18212 18213)
[ "$H" = 10 ] && [ -z "$O" ] && ok "eager: 10 x jset@B jdel@C (root) - deleted, absent everywhere" \
	|| bad "eager: jset@B jdel@C - $H of 10 answered true; still present: ${O:-none}"
# a key nobody has: false, and the second miss is free
P0=$(stat 18212 pull_sent)
R1=$(one 18212 '{"method":"del","col":"0","key":"nosuch"}')
P1=$(stat 18212 pull_sent)
R2=$(one 18212 '{"method":"del","col":"0","key":"nosuch"}')
P2=$(stat 18212 pull_sent)
echo "$R1$R2" | grep -q true && bad "eager: a delete of a key nobody has answered true ($R1 $R2)" \
	|| { [ $((P1 - P0)) -ge 1 ] && [ $((P2 - P1)) = 0 ] \
		&& ok "eager: a delete of a key nobody has answers false; the first miss asked the fleet (pull_sent +$((P1 - P0))), the second within the negative window asked nobody" \
		|| bad "eager: the miss probes are wrong (pull_sent +$((P1 - P0)) then +$((P2 - P1)); want >=1 then 0)"; }
# control: the delete a second after the write, B holds it by then
one 18211 '{"method":"set","col":"0","key":"slow","value":"v","ttl":120}' > /dev/null
sleep 1
R=$(one 18212 '{"method":"del","col":"0","key":"slow"}')
echo "$R" | grep -q true && ok "eager control: a delete a second after the write, on a node that holds the copy, still answers true" \
	|| bad "eager control: the slow delete answered $R"
BR=$(cat "$D"/n1[123].log | grep -c "birth race")
[ "$BR" = 0 ] && ok "no probe was mistaken for a birth race (several holders answering yes is eager's normal state)" \
	|| bad "$BR 'birth race' lines: a second holder's yes was read as a conflict and a healthy replica demoted"
stop_arm 1

# ---- store ---------------------------------------------------------
node 2 1 "mode = store"; node 2 2 "mode = store"
start 2 1; start 2 2
sleep 1
H=$(pairs native 18221 18222 10 st); O=$(absent 10 st 18221 18222)
[ "$H" = 10 ] && [ -z "$O" ] && ok "store: 10 x set@A del@B - deleted where it lived, absent on both nodes" \
	|| bad "store: set@A del@B - $H of 10 answered true; still present: ${O:-none}"
stop_arm 2

# ---- spread --------------------------------------------------------
for n in 1 2 3; do node 3 $n "mode = spread
replicas = 2"; done
for n in 1 2 3; do start 3 $n; done
sleep 1
HA=0
for n in 1 2 3; do
	one 18231 "{\"method\":\"set\",\"col\":\"0\",\"key\":\"sp$n\",\"value\":\"v\",\"ttl\":120}" > /dev/null
done
sleep 1
for n in 1 2 3; do
	one 1823$n "{\"method\":\"del\",\"col\":\"0\",\"key\":\"sp$n\"}" | grep -q true && HA=$((HA + 1))
done
sleep 1
N=$(( $(entries 18231) + $(entries 18232) + $(entries 18233) ))
[ "$HA" = 3 ] && [ "$N" = 0 ] && ok "spread K=2: a delete via every node answered true and no copy survived (fleet-wide entries $N)" \
	|| bad "spread: $HA of 3 deletes answered true, fleet-wide entries $N (want 0)"
stop_arm 3

echo "delprobetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
