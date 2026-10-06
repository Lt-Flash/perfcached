#!/bin/sh
# upstreamclustertest.sh - S331 read-through in an EAGER fleet (DESIGN 12ix).
#
# Three nodes, one upstream (test/fakeupstream.py - it fails the suite on
# any write).  A fetched copy is NODE-LOCAL (each node's shadow); what
# changes a key is a real write, and replicates:
#   1  the fleet forms with the companion collection on every node
#   2  each node fetches a key for itself - one fetch per node
#   3  a mutation on one node (INCR of an upstream counter) PROMOTES it:
#      the others read the replicated record and never fetch it
#   4  a key another node holds a fetched copy of, deleted elsewhere,
#      reads as gone there too - the no-fall entry replicated, and the
#      shadow's gate refuses the stale copy
#   5  a plain SET on one node and a DEL on another: gone everywhere
#   6  nothing was written upstream
# Usage: test/upstreamclustertest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=18770
for p in $UP 18771 18772 18773 18781 18782 18783 18791 18792 18793; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "upstreamclustertest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcupc.XXXXXX)
FPID=
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "$FPID" ] && kill -9 $FPID 2>/dev/null; rm -rf "$D"' EXIT INT TERM

mkdir -p "$D/up"
cat > "$D/up/data.json" <<'J'
{"tok:a": {"type": "string", "value": "alpha"},
 "tok:cnt": {"type": "string", "value": "41"},
 "tok:d": {"type": "string", "value": "dval"},
 "tok:o": {"type": "string", "value": "upstream-o"}}
J
echo '{"password": null, "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

conf() { # conf <n>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = upc-client-secret
cluster = upc-cluster-secret
[listen]
tcp = 127.0.0.1:1877$1
resp = 127.0.0.1:1878$1
http = 127.0.0.1:1879$1
plaintext = loopback
[cluster]
multicast = 239.255.80.57:18799
advertise = 127.0.57.$1
mode = eager
collections = 0
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
C
	chmod 600 "$D/n$1.conf"
}
for n in 1 2 3; do
	conf $n
	"$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
	sleep 0.4
done

up() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:1879%s/stats" % sys.argv[1],timeout=3).read())
print(r["cluster"]["peers_up"], r["state"], " ".join(sorted(c["name"] for c in r["collections"])))' "$1" 2>/dev/null || echo "?"; }
i=0
while [ $i -lt 80 ]; do
	[ "$(up 1 | cut -d' ' -f1-2)$(up 2 | cut -d' ' -f1-2)$(up 3 | cut -d' ' -f1-2)" = "2 ready2 ready2 ready" ] && break
	sleep 0.5; i=$((i+1))
done

cat > "$D/rc.py" <<'P'
import socket, sys
def enc(cmd):
    parts = cmd.split(" ")
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p.encode()) for p in parts)
def rd(f):
    l = f.readline()
    t, rest = l[:1], l[1:-2]
    if t in (b"+", b"-"): return (t + rest).decode()
    if t == b":": return int(rest)
    if t == b"$":
        n = int(rest)
        return None if n < 0 else f.read(n + 2)[:-2].decode("latin-1")
    if t == b"*":
        n = int(rest)
        return None if n < 0 else [rd(f) for _ in range(n)]
    return "?" + l.decode("latin-1")
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
s.sendall(b"".join(enc(c) for c in sys.argv[2:]))
f = s.makefile("rb")
for _ in sys.argv[2:]:
    print(repr(rd(f)))
P
rc() { n=$1; shift; python3 "$D/rc.py" 1878$n "$@" 2>&1; }
cnt() { c=$(grep -c "^TYPE $1\$" "$D/up/counts" 2>/dev/null); echo "${c:-0}"; }
# wait_for <seconds> <shell condition>
wait_for() { k=0; while [ $k -lt $(($1 * 4)) ]; do eval "$2" && return 0; sleep 0.25; k=$((k+1)); done; return 1; }

echo "--- 1: the fleet"
U1=$(up 1); U2=$(up 2); U3=$(up 3)
case "$U1|$U2|$U3" in
"2 ready 0 0.nofall|2 ready 0 0.nofall|2 ready 0 0.nofall") ok "three nodes ready, each with 0 and its companion 0.nofall" ;;
*) bad "fleet: [$U1] [$U2] [$U3]" ;; esac

echo "--- 2: a fetched copy is node-local"
A=$(rc 1 'GET tok:a')$(rc 2 'GET tok:a')
[ "$A" = "'alpha''alpha'" ] && [ "$(cnt tok:a)" = 2 ] && ok "two nodes read tok:a: each fetched it once for itself" \
	|| bad "tok:a: $A, fetched $(cnt tok:a) time(s)"

echo "--- 3: a mutation promotes, and the record replicates"
# read on the others only once the promotion has REPLICATED: inside the
# eager window a read there falls through and may answer the upstream's
# older value (DESIGN 12ix, "replication window") - that is not under test
nfe() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:1879%s/stats" % sys.argv[1],timeout=3).read())
print([c for c in r["collections"] if c["name"]=="0.nofall"][0]["entries"])' "$1" 2>/dev/null || echo 0; }
N2=$(nfe 2); N3=$(nfe 3)
I=$(rc 1 'INCR tok:cnt')
wait_for 5 '[ "$(nfe 2)" -gt "$N2" ] && [ "$(nfe 3)" -gt "$N3" ]'
G=$(rc 2 'GET tok:cnt')$(rc 3 'GET tok:cnt')
[ "$I" = 42 ] && [ "$G" = "'42''42'" ] && [ "$(cnt tok:cnt)" = 1 ] \
	&& ok "INCR on node 1 (41 -> 42); once replicated, nodes 2 and 3 read 42 without fetching" \
	|| bad "tok:cnt: INCR $I, others $G, fetched $(cnt tok:cnt) time(s)"

echo "--- 4: a delete on one node beats another node's fetched copy"
P=$(rc 2 'GET tok:d')
X=$(rc 1 'DEL tok:d')
wait_for 5 '[ "$(rc 2 "GET tok:d")" = None ]'
Y=$(rc 2 'GET tok:d')$(rc 3 'GET tok:d')
[ "$P" = "'dval'" ] && [ "$X" = 1 ] && [ "$Y" = "NoneNone" ] \
	&& ok "node 2 held a fetched copy; DEL on node 1 answered 1; nodes 2 and 3 read it gone" \
	|| bad "tok:d: node 2 first $P, DEL $X, then $Y"

echo "--- 5: SET on one node, DEL on another"
rc 3 'SET tok:o mine' > /dev/null
wait_for 5 '[ "$(rc 1 "GET tok:o")" = "'"'mine'"'" ]'
O1=$(rc 1 'GET tok:o')
sleep 0.5
D2=$(rc 2 'DEL tok:o')
wait_for 5 '[ "$(rc 1 "GET tok:o")$(rc 3 "GET tok:o")" = NoneNone ]'
O2=$(rc 1 'GET tok:o')$(rc 2 'GET tok:o')$(rc 3 'GET tok:o')
[ "$O1" = "'mine'" ] && [ "$D2" = 1 ] && [ "$O2" = "NoneNoneNone" ] \
	&& ok "SET on node 3 read on node 1; DEL on node 2; gone on all three" \
	|| bad "tok:o: node 1 $O1, DEL $D2, then $O2"

echo "--- 6: nothing written upstream"
[ ! -s "$D/up/writes" ] && ok "the upstream received no write command" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"
E=$(cat "$D"/n*.log | grep -cE " (ERROR|CRIT)")
[ "$E" = 0 ] && ok "no ERROR or CRIT on any node" || bad "$E ERROR/CRIT line(s): $(cat "$D"/n*.log | grep -m1 -E ' (ERROR|CRIT)' | cut -c1-120)"

echo "upstreamclustertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
