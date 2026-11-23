#!/bin/sh
# upownedtest.sh - fleet_owned counts a key only when it was unmarked at the walk before too, so a
# record caught between its write-through push and its "t" never shows on a healthy fleet; and an
# invalidation dropping a peer's copy does not take the mark of a fresher copy pushed meanwhile.
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupown.XXXXXX)
UP=20810
FPID= P1ID= P2ID= LPID=
trap 'for p in $LPID $P1ID $P2ID $FPID; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $UP 20819 20821 20822 20831 20832 20841 20842; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upownedtest: port $p already bound" >&2; exit 1; }
done
mkdir -p "$D/up"
echo '{}' > "$D/up/data.json"
echo '{"role": "master", "accept_writes": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 & FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

node() { # node <n> - tcp 2082n, http 2083n, resp 2084n
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = upown-client-secret
cluster = upown-cluster-secret
[listen]
tcp = 127.0.0.1:2082$1
http = 127.0.0.1:2083$1
resp = 127.0.0.1:2084$1
resp_collections = 0
plaintext = loopback
[cluster]
multicast = 239.255.79.81:20819
advertise = 127.0.88.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 12
upstream = redis://127.0.0.1:$UP
upstream_writes = through
upstream_prefixes = w:*
upstream_timeout_ms = 2000
C
	chmod 600 "$D/n$1.conf"
	: > "$D/n$1.log"
	env ${NODE_ENV:-} PERFCACHED_TEST_WT_OWNED_MS=300 "$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log")"; echo "upownedtest: $pass passed, $fail failed"; exit 1; }
}
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:2083%s/stats" % sys.argv[1], timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null; }
WT="[c for c in s['collections'] if c['name']=='0'][0]['upstream']['write_through']"
node 1; node 2
i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']") $(st 2 "s['cluster']['peers_up']")" != "1 1" ]; do sleep 0.2; i=$((i+1)); done
for n in 1 2; do       # ready too: a write before it is refused LOADING (113382)
	i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state'], $WT['target'], $WT['bcast']")" != "('ready', '127.0.0.1:$UP', 'on')" ]; do sleep 0.2; i=$((i+1)); done
done
[ "$(st 1 "s['cluster']['peers_up']") $(st 1 "$WT['target'], $WT['bcast']") $(st 2 "$WT['target'], $WT['bcast']")" = "1 ('127.0.0.1:$UP', 'on') ('127.0.0.1:$UP', 'on')" ] ||
	bad "the fleet: peers_up $(st 1 "s['cluster']['peers_up']"), targets and broadcast tracking $(st 1 "$WT['target'], $WT['bcast']") / $(st 2 "$WT['target'], $WT['bcast']")"
f0=$(st 1 "$WT['forwarded']")

# the load: new keys through node 1, 16 a pipeline, at most 1,500 a second for 8 s (the fake
# master takes a few dozen a second written through); the first error's text kept
python3 - "$D/load.out" <<'P' &
import socket, sys, time
k = socket.create_connection(("127.0.0.1", 20841), 10); f = k.makefile("rb")
n, t0, err, first = 0, time.time(), 0, ""
while time.time() < t0 + 8 and err < 100:
    k.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nw:%d\r\n$2\r\nv1\r\n" % (len(b"w:%d" % (n + j)), n + j) for j in range(16)))
    for j in range(16):
        l = f.readline()
        if not l.startswith(b"+"):
            err += 1
            first = first or l.decode("latin-1").strip()[:160]
    n += 16
    lag = t0 + n / 1500.0 - time.time()
    if lag > 0: time.sleep(lag)
open(sys.argv[1], "w").write("%d %d %s\n" % (n, err, first.replace(" ", "_")))
P
LPID=$!
samples= nz=0 mx=0 s=0
while kill -0 $LPID 2>/dev/null && [ $s -lt 60 ]; do
	sleep 0.25
	for n in 1 2; do
		o=$(st $n "$WT['fleet_owned']")
		samples="$samples $n:$o"
		case "$o" in ''|None|0) ;; *) nz=$((nz + 1)); [ "$o" -gt "$mx" ] && mx=$o;; esac
	done
	s=$((s + 1))
done
wait $LPID 2>/dev/null; LPID=
set -- $(cat "$D/load.out" 2>/dev/null) x x x; nw=$1 ne=$2 fe=$3
fw=$(( $(st 1 "$WT['forwarded']") - f0 ))
[ "$ne" = 0 ] && [ "$nw" -ge 100 ] && [ "$fw" -ge "$nw" ] &&
	ok "A. $nw new keys written through node 1 in 8 s, $fw forwarded, no error" ||
	bad "A. the premise: $nw keys written, $ne errors (the first: $fe), $fw forwarded (at least the keys); node 1: $(grep -E 'WARN|ERR' "$D/n1.log" | tail -2 | cut -c1-160 | tr '\n' '|') fake: $(tail -2 "$D/fake.log" | cut -c1-120 | tr '\n' '|')"
[ "$nz" = 0 ] && [ $s -ge 10 ] && ok "B. fleet_owned 0 at every one of $((s * 2)) samples under the load" ||
	bad "B. fleet_owned under the load: $nz of $((s * 2)) samples above 0, at most $mx - in flight, not the fleet's (samples:$(echo "$samples" | cut -c1-300))"
sleep 1
o1=$(st 1 "$WT['fleet_owned']"); o2=$(st 2 "$WT['fleet_owned']")
[ "$o1 $o2" = "0 0" ] && ok "C. a second after the writes: fleet_owned 0 on both" ||
	bad "C. after the writes: fleet_owned $o1 / $o2 (0 / 0)"

# D: node 2 again, its invalidations held 500 ms between their two drops; each key is written
# through node 1 three times - v1, v2 300 ms later, v2 again 150 ms after that (the master does
# not change: no invalidation) - so a push lands inside an invalidation of a copy node 2 holds.
# No copy may stay without its "t".
kill $P2ID 2>/dev/null; wait $P2ID 2>/dev/null; P2ID=
NODE_ENV=PERFCACHED_TEST_INV_GAP_US=500000 node 2
i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']") $(st 2 "s['cluster']['peers_up'], s['state'], $WT['bcast']")" != "1 (1, 'ready', 'on')" ]; do sleep 0.2; i=$((i+1)); done
v0=$(st 2 "$WT['invalidated']")
python3 - <<'G'
import socket, time
k = socket.create_connection(("127.0.0.1", 20841), 10); f = k.makefile("rb")
for i in range(20):
    key = b"w:gap:%d" % i
    for v, d in ((b"v1", 0.3), (b"v2", 0.15), (b"v2", 0.7)):
        k.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$2\r\n%s\r\n" % (len(key), key, v))
        f.readline()
        time.sleep(d)
G
sleep 2
o2=$(st 2 "$WT['fleet_owned']"); v1=$(st 2 "$WT['invalidated']")
[ $(( ${v1:-0} - ${v0:-0} )) -ge 5 ] 2>/dev/null ||
	bad "D. the premise: node 2 dropped $(( ${v1:-0} - ${v0:-0} )) copies on an invalidation (at least 5 of 20 keys)"
[ "$o2" = 0 ] && ok "D. an invalidation held across a fresher push: fleet_owned 0 on node 2" ||
	bad "D. an invalidation held across a fresher push: fleet_owned $o2 on node 2 (0) - the fresher copy lost its mark"
for n in 1 2; do grep -h 'an fsync has been stuck' "$D/n$n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"; grep "CRIT" "$D/n$n.log" | grep -qv 'an fsync has been stuck' && bad "node $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v 'an fsync has been stuck' | head -n 2 | tr '\n' ' ' | cut -c1-200)"; done
echo "upownedtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
