#!/bin/sh
# upclustertest.sh - a Redis Cluster as a read-through upstream from one seed: six fakes (3 shards x master + replica),
# each serving only its shard's slots; upstream_role replica, upstream_master_fallback yes.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupcl.XXXXXX)
PM1=19761 PM2=19762 PM3=19763 PR1=19764 PR2=19765 PR3=19766 PP=19767 TCP=19768 RESP=19769 HTTP=19770
PID= FK=
trap 'kill $PID 2>/dev/null; for p in $FK; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM1 $PM2 $PM3 $PR1 $PR2 $PR3 $PP $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upclustertest: port $p already bound" >&2; exit 1; }
done
KEYS='["k%d" % i for i in range(60)] + ["{t}a", "{t}b", "{u}a", "{n}a"]'
for n in m1 m2 m3 r1 r2 r3 p; do
	mkdir -p "$D/$n"
	python3 -c 'import json,sys; json.dump({k: {"type": "string", "value": sys.argv[1]} for k in '"$KEYS"'}, open(sys.argv[2], "w"))' "$n" "$D/$n/data.json"
done
# topo <shard 1: both | promoted> [moved slots, a,b] [the moved slots reported, - for all] [ask key]: every
# fake's ctl.json; a moved slot leaves its shard for the next
topo() {
	python3 - "$D" "$1" "${2:-}" "${3:--}" "${4:-}" $PM1 $PM2 $PM3 $PR1 $PR2 $PR3 <<'PY'
import json, os, sys
d, s1, mv, rp, askkey = sys.argv[1:6]
pm, pr = [int(x) for x in sys.argv[6:9]], [int(x) for x in sys.argv[9:12]]
H = "127.0.0.1"
rng = [[0, 5460], [5461, 10922], [10923, 16383]]
def shards(moved):
    sh = [{"slots": [r], "nodes": [[H, pm[i], "master"], [H, pr[i], "replica"]]} for i, r in enumerate(rng)]
    if s1 == "promoted":
        sh[0]["nodes"] = [[H, pr[0], "master"]]
    for m in moved:
        i = next(i for i, r in enumerate(rng) if r[0] <= m <= r[1])
        out = []
        for lo, hi in sh[i]["slots"]:
            out += [x for x in ([lo, m - 1], [m + 1, hi]) if x[0] <= x[1]] if lo <= m <= hi else [[lo, hi]]
        sh[i]["slots"] = out
        sh[(i + 1) % 3]["slots"].append([m, m])
    return sh
def slots_of(spec):     # "a,b" or "a:b:step" - a range every step slots
    out = []
    for x in spec.split(","):
        if ":" in x:
            a, b, st = (int(y) for y in x.split(":"))
            out += list(range(a, b, st))
        elif x:
            out.append(int(x))
    return out
moved = slots_of(mv)
sh = shards(moved)
rep = shards(slots_of(rp)) if rp != "-" else None
names = ["m1", "m2", "m3", "r1", "r2", "r3"]
for k, n in enumerate(names):
    shard = k % 3
    cl = {"shards": sh, "slots_only": k == 0}
    if rep:
        cl["report"] = rep
    ctl = {"redis_mode": "cluster", "role": "master" if k < 3 else "replica", "cluster": cl,
           "accept_writes": True}
    if k >= 3:
        ctl.update(master_host=H, master_port=pm[shard])
    if n == "r1" and s1 == "promoted":
        ctl["role"] = "master"
    if n == "m1" and s1 == "promoted":
        ctl["down"] = True
    if os.environ.get("EMPTYHOST"):
        cl["moved_empty_host"] = True
    if n == os.environ.get("DOWN", ""):
        ctl["down"] = True
    if askkey:      # its shard says -ASK, the next shard's master imports it (ASKTO: that port)
        from_ = json.load(open(d + "/askshard"))
        if shard == from_:
            cl["ask"] = {askkey: [H, int(os.environ.get("ASKTO") or pm[(from_ + 1) % 3])]}
        if n == names[(from_ + 1) % 3]:
            cl["importing"] = [askkey]
    json.dump(ctl, open(d + "/" + n + "/ctl.json.tmp", "w"))
for n in names:
    os.rename(d + "/" + n + "/ctl.json.tmp", d + "/" + n + "/ctl.json")
PY
}
# the slot and shard (0-2) of a key, as the fakes compute them
slot() { python3 - "$1" <<'PY'
import sys
k = sys.argv[1].encode()
o = k.find(b"{")
if o >= 0:
    c = k.find(b"}", o + 1)
    if c > o + 1:
        k = k[o + 1:c]
crc = 0
for b in k:
    crc ^= b << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
s = crc % 16384
print(s, 0 if s <= 5460 else 1 if s <= 10922 else 2)
PY
}
topo both
echo '{"role": "master", "redis_mode": "cluster", "perfcached_version": "0.7.5"}' > "$D/p/ctl.json"
for x in "m1 $PM1" "m2 $PM2" "m3 $PM3" "r1 $PR1" "r2 $PR2" "r3 $PR3" "p $PP"; do
	set -- $x
	python3 "$T/fakeupstream.py" $2 "$D/$1" > "$D/f$1.log" 2>&1 & FK="$FK $!"
done
i=0; while [ $i -lt 50 ] && [ "$(ls "$D"/*/ready 2>/dev/null | wc -l)" -lt 7 ]; do sleep 0.1; i=$((i+1)); done
mkdir -p "$D/state"
cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = upcl-client-secret
enable = upcl-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PM1
upstream_prefixes = *
upstream_role = replica
upstream_master_fallback = yes
upstream_timeout_ms = 2000
upstream_negative_ms = 1000
[collection 1]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PM2/1
upstream_prefixes = *
upstream_timeout_ms = 1000
[collection pc]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PP
upstream_prefixes = *
upstream_allow_perfcached = yes
[collection hn]
buckets_log2 = 8
upstream = redis://localhost:$PM2
upstream_prefixes = *
upstream_role = replica
upstream_master_fallback = yes
[collection wt]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PM1
upstream_prefixes = *
upstream_writes = through
upstream_role = replica
upstream_master_fallback = yes
upstream_timeout_ms = 2000
C
chmod 600 "$D/n.conf"
start() {
	: > "$D/$1"
	PERFCACHED_TEST_CL_REFRESH_MS=2000 "$BIN" -f "$D/n.conf" > "$D/$1" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1" || { bad "the node did not start: $(tail -3 "$D/$1")"; exit 1; }
}
# st <collection> <python expr over u (its upstream block) and a (its upstreams by addr)>
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u=[c for c in s["collections"] if c["name"]==sys.argv[1]][0]["upstream"]
a={x["addr"]: x for x in u["upstreams"]}
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null; }
mapped() { # mapped <rounds after> [collection]: up to 25 s (a round every 2 s here) for every slot served, after that many rounds
	k=0; while [ $k -lt 125 ]; do
		[ "$(st "${2:-0}" '"16384 of 16384" in u["redis_cluster"]["said"] and u["redis_cluster"]["rounds"] > '"${1:-0}")" = True ] && return 0
		sleep 0.2; k=$((k+1))
	done; return 1
}
# run <db> <command words...>: its reply, one line (a bulk's value)
run() { python3 - $RESP "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
def cmd(*a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    l = f.readline()
    if l.startswith(b"$") and not l.startswith(b"$-1"):
        return f.read(int(l[1:]) + 2)[:-2].decode()
    return l.decode().strip()
cmd(b"SELECT", sys.argv[2].encode())
print(cmd(*[x.encode() for x in sys.argv[3:]]))
PY
}
# get <db> <key...>: each value, space-separated, read through the RESP door
get() { python3 - $RESP "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
def cmd(*a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    l = f.readline()
    if l.startswith(b"$") and not l.startswith(b"$-1"):
        return f.read(int(l[1:]) + 2)[:-2].decode()
    return l.decode().strip()
cmd(b"SELECT", sys.argv[2].encode())
print(" ".join(cmd(b"GET", k.encode()).replace(" ", "_") for k in sys.argv[3:]))
PY
}
start n.log

# ---- A. discovery: the seed refuses SHARDS, so A-D map from CLUSTER SLOTS - 3 shards, 3 masters, 6 nodes, every
# slot served; the five not named are added (origin cluster), each with its shard and slots in /stats
mapped 0 && w=$(st 0 'u["redis_cluster"]["said"], len(a), sorted(x["origin"] for x in a.values())') || w="no map: $(st 0 'u.get("redis_cluster")')"
case "$w" in
"('3 shard(s), 3 master(s), 6 node(s); 16384 of 16384 slots served', 6, ['cluster', 'cluster', 'cluster', 'cluster', 'cluster', 'configured'])")
	ok "A. one seed: 3 shards, 3 masters, 6 nodes, every slot served; five nodes added";;
*) bad "A. discovery: $w";;
esac
w=$(st 0 'a["127.0.0.1:'$PR2'"]["shard"], a["127.0.0.1:'$PR2'"]["slots"], a["127.0.0.1:'$PM3'"]["slots"]')
[ "$w" = "(1, '5461-10922', '10923-16383')" ] && ok "A. each node's shard and slots in /stats" || bad "A. shard and slots: $w"

# ---- B. reads by slot: 30 keys, each from its shard's replica, which got READONLY; nothing MOVED
KS=$(python3 -c 'print(" ".join("k%d" % i for i in range(30)))')
WANT=$(for k in $KS; do set -- $(slot "$k"); echo "r$(($2 + 1))"; done | tr '\n' ' ' | sed 's/ $//')
GOT=$(get 0 $KS)
[ "$GOT" = "$WANT" ] && ok "B. 30 keys, each from its shard's replica" || bad "B. reads: got '$GOT', want '$WANT'"
w=$(st 0 'u["redis_cluster"]["moved"], u["redis_cluster"]["asked"]')
n=$(cat "$D/r1/counts" "$D/r2/counts" "$D/r3/counts" 2>/dev/null | grep -c '^READONLY')
[ "$w" = "(0, 0)" ] && [ "$n" -ge 3 ] && ok "B. nothing MOVED or ASKed; the replicas got READONLY ($n)" || bad "B. moved/asked $w, READONLY $n"

# ---- C. a reshard: {t}a answers -MOVED, is read from the new master once, and the map is asked again;
# {t}b then comes from the new shard's replica
set -- $(slot "{t}a"); TS=$1 TO=$(( ($2 + 1) % 3 + 1 ))
R0=$(st 0 'u["redis_cluster"]["rounds"]')
topo both "$TS"
GOT=$(get 0 "{t}a")
w=$(st 0 'u["redis_cluster"]["moved"]')
[ "$GOT" = "m$TO" ] && [ "$w" = 1 ] && ok "C. slot $TS moved: {t}a answered -MOVED and was read from the new master m$TO" ||
	bad "C. after the reshard: {t}a = '$GOT' (want m$TO), moved $w"
mapped "$R0"
GOT=$(get 0 "{t}b")
w=$(st 0 'u["redis_cluster"]["moved"], a["127.0.0.1:'"$(eval echo \$PR$TO)"'"]["slots"]')
[ "$GOT" = "r$TO" ] && [ "${w%%,*}" = "(1" ] && ok "C. the map asked again: {t}b from r$TO, nothing more MOVED ($w)" ||
	bad "C. {t}b = '$GOT' (want r$TO), moved/slots $w"

# ---- D. a migration: {u}a answers -ASK naming another master, read there after ASKING; the map unchanged
set -- $(slot "{u}a"); UF=$2; UT=$(( (UF + 1) % 3 + 1 ))
echo "$UF" > "$D/askshard"
topo both "$TS" - "{u}a"
GOT=$(get 0 "{u}a")
w=$(st 0 'u["redis_cluster"]["asked"], u["redis_cluster"]["moved"]')
n=$(grep -c '^ASKING' "$D/m$UT/counts" 2>/dev/null)
[ "$GOT" = "m$UT" ] && [ "$w" = "(1, 1)" ] && [ "$n" -ge 1 ] && ok "D. {u}a answered -ASK: read from m$UT after ASKING ($n), the map unchanged" ||
	bad "D. {u}a = '$GOT' (want m$UT), asked/moved $w, ASKING at m$UT $n"
# an -ASK naming a node the cluster does not list is not dialled: it would get the collection's credentials
set -- $(slot "{v}a"); echo "$2" > "$D/askshard"
ASKTO=$PP topo both "$TS" - "{v}a"
GOT=$(get 0 "{v}a")
w=$(st 0 '"127.0.0.1:'$PP'" in a')
[ "$w" = False ] && ok "D. -ASK naming 127.0.0.1:$PP, a node the cluster does not list: not dialled, not added ('$GOT')" ||
	bad "D. -ASK naming an unlisted node: in the upstreams '$w', {v}a read '$GOT'"
topo both "$TS"

# ---- E. a failover: shard 1's master down, its replica promoted (and answering SHARDS) - the next round maps
# it, its keys come from it
K1=$(for i in $(seq 30 59); do set -- $(slot "k$i"); [ "$2" = 0 ] && echo "k$i"; done | head -3 | tr '\n' ' ')
R0=$(st 0 'u["redis_cluster"]["rounds"]')
topo promoted "$TS"
mapped "$((R0 + 1))"
GOT=$(get 0 $K1)
# the old master is found down by its next ROLE check or PING (every 10 s)
k=0; while [ $k -lt 75 ] && [ "$(st 0 'a["127.0.0.1:'$PM1'"]["state"]')" != "not used" ]; do sleep 0.2; k=$((k+1)); done
w=$(st 0 'a["127.0.0.1:'$PR1'"]["shard"], a["127.0.0.1:'$PM1'"]["state"], u["redis_cluster"]["said"]')
[ "$GOT" = "r1 r1 r1" ] && [ "$w" = "(0, 'not used', '3 shard(s), 3 master(s), 5 node(s); 16384 of 16384 slots served')" ] &&
	ok "E. shard 1 failed over: mapped to the promoted replica, its keys from it, the old master not used" ||
	bad "E. after the failover: '$GOT' (want r1 r1 r1), $w"

# ---- F. restarted with the seed down: the topology kept in state_dir is tried, the map comes back, keys read
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
start n2.log
K2=$(python3 -c 'print(" ".join("k%d" % i for i in range(40, 46)))')
WANT=$(for k in $K2; do set -- $(slot "$k"); echo "r$(($2 + 1))"; done | tr '\n' ' ' | sed 's/ $//')
mapped 0; GOT=$(get 0 $K2)
grep -q "the last topology - 5 server(s), 0 sentinel(s) - tried at the start" "$D/n2.log" && [ "$GOT" = "$WANT" ] &&
	ok "F. restarted with the seed down: the kept topology tried, keys read by slot" ||
	bad "F. restart: '$GOT' (want '$WANT'); $(grep -m1 'last topology' "$D/n2.log")"

# ---- G. a database other than 0 on a cluster node: SELECT refused, the log says a cluster has database 0 only
get 1 kx > /dev/null
grep -q "SELECT 1 refused by 127.0.0.1:$PM2 - a Redis Cluster has database 0 only" "$D/n2.log" &&
	ok "G. /1 on a cluster node: refused, and the log says a cluster has database 0 only" ||
	bad "G. the log: $(grep -m1 'SELECT 1' "$D/n2.log")"

# ---- H. a perfcached saying redis_mode:cluster is not a Redis Cluster: allowed (upstream_allow_perfcached),
# every key from it, no cluster block
GOT=$(get pc k0 k1 k2 k3 k4)
w=$(st pc 'a["127.0.0.1:'$PP'"]["state"], a["127.0.0.1:'$PP'"]["redis_cluster"], a["127.0.0.1:'$PP'"]["perfcached"], "redis_cluster" in u')
[ "$GOT" = "p p p p p" ] && [ "$w" = "('serving', False, True, False)" ] &&
	ok "H. a perfcached saying redis_mode:cluster is a perfcached: allowed, every key from it" ||
	bad "H. a perfcached saying redis_mode:cluster: '$GOT', /stats $w"

# ---- I. write-through (a second collection on the same seed): each SET goes to its slot's master alone;
# shard 1's master is r1 since E
owner() { set -- $(slot "$1"); case $2 in 0) echo r1;; 1) echo m2;; 2) echo m3;; esac; }
bad_i=""
# its own map first: a SET before wt has learnt the masters is refused, not sent (GitHub rc4: w0)
mapped 0 wt || bad_i=" wt never mapped every slot"
for k in w0 w1 w2 w3 w4 w5; do
	r=$(run wt SET "$k" "v-$k"); o=$(owner "$k")
	n=$(grep -c "^SET $k " "$D/$o/writes" 2>/dev/null); a=$(cat "$D"/*/writes 2>/dev/null | grep -c "^SET $k ")
	[ "$r" = "+OK" ] && [ "$n" = 1 ] && [ "$a" = 1 ] || bad_i="$bad_i $k:$r:$o:$n/$a"
done
GOT=$(get wt w0 w3 w5)
[ -z "$bad_i" ] && [ "$GOT" = "v-w0 v-w3 v-w5" ] && ok "I. write-through: each SET to its slot's master alone, the fleet holds it" ||
	bad "I. writes:$bad_i; read back '$GOT'"

# ---- J. a DEL of keys in two slots is refused as CROSSSLOT, nothing sent; one within a hash tag goes
A=w0; B=$(for k in w1 w2 w3 w4 w5; do [ "$(slot $k | cut -d' ' -f1)" != "$(slot $A | cut -d' ' -f1)" ] && { echo $k; break; }; done)
r=$(run wt DEL "$A" "$B"); n=$(cat "$D"/*/writes 2>/dev/null | grep -c "^DEL ")
r2=$(run wt DEL "{w}a" "{w}b"); n2=$(cat "$D"/*/writes 2>/dev/null | grep -c "^DEL {w}a {w}b")
case "$r" in *CROSSSLOT*) c=1;; *) c=0;; esac
[ "$c" = 1 ] && [ "$n" = 0 ] && [ "$r2" = ":0" ] && [ "$n2" = 1 ] &&
	ok "J. a DEL of two slots refused as CROSSSLOT, nothing sent; one within a hash tag goes" ||
	bad "J. DEL $A $B: '$r' ($n sent); DEL {w}a {w}b: '$r2' ($n2 sent)"

# ---- K. a write to a slot the cluster moved, the map not told yet: -MOVED, re-run on the new master, once
set -- $(slot "{w}x"); WS=$1 WO=$(( ($2 + 1) % 3 ))
case $WO in 0) WN=r1;; 1) WN=m2;; 2) WN=m3;; esac
M0=$(st wt 'u["redis_cluster"]["moved"]')
topo promoted "$TS,$WS" "$TS"      # routed by the new map, the old one still said
r=$(run wt SET "{w}x" "v-x"); n=$(grep -c '^SET {w}x ' "$D/$WN/writes" 2>/dev/null)
w=$(st wt 'u["redis_cluster"]["moved"]')
# the write's MOVED, and perhaps a read's after it - the map the cluster says still lags
[ "$r" = "+OK" ] && [ "$n" = 1 ] && [ "$w" -gt "$M0" ] &&
	ok "K. a write to a slot that moved: MOVED, re-run on $WN, applied once there" ||
	bad "K. SET {w}x: '$r', at $WN $n, moved $M0 -> $w"
topo promoted "$TS,$WS"

# ---- L. the drain sweep SCANs each shard on a server that serves it (the replicas), every key read once
dr() { python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),10))
pcnative.call(f,"enable",secret="upcl-enable-secret")
r=pcnative.call(f,"updrain",col="0")
print(json.dumps(r.get("result", r.get("error"))))' $TCP 2>&1; }
WANT=$(python3 - "$D" <<'PY'
import json, sys
d = sys.argv[1]
def slot(k):
    o = k.find(b"{")
    if o >= 0:
        c = k.find(b"}", o + 1)
        if c > o + 1:
            k = k[o + 1:c]
    crc = 0
    for b in k:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc % 16384
n = 0
for node, sh in (("r1", 0), ("r2", 1), ("r3", 2)):
    cl = json.load(open(d + "/" + node + "/ctl.json"))["cluster"]["shards"][sh]
    n += sum(1 for k in json.load(open(d + "/" + node + "/data.json"))
             if any(lo <= slot(k.encode()) <= hi for lo, hi in cl["slots"]))
print(n)
PY
)
SC=$(for n in r1 r2 r3 m2 m3; do printf '%s ' "$(grep -c '^SCAN ' "$D/$n/counts" 2>/dev/null)"; done)
R=$(dr)
k=0; while [ "$(st 0 'u["drain"]["state"]')" = running ] && [ $k -lt 150 ]; do sleep 0.2; k=$((k+1)); done
w=$(st 0 'u["drain"]["state"], u["drain"]["read"], u["drain"]["errors"]')
SC2=$(for n in r1 r2 r3 m2 m3; do printf '%s ' "$(grep -c '^SCAN ' "$D/$n/counts" 2>/dev/null)"; done)
set -- $SC; a1=$1 a2=$2 a3=$3 a4=$4 a5=$5; set -- $SC2
case "$R" in *started*) s=1;; *) s=0;; esac
[ "$s" = 1 ] && [ "$w" = "('done', $WANT, 0)" ] && [ "$1" -gt "$a1" ] && [ "$2" -gt "$a2" ] && [ "$3" -gt "$a3" ] &&
	[ "$4" = "$a4" ] && [ "$5" = "$a5" ] && ok "L. the drain sweep SCANs each shard on its replica: $WANT keys read, no error" ||
	bad "L. drain: '$R', $w (want done, $WANT, 0); SCANs before '$SC' after '$SC2' (r1 r2 r3 m2 m3)"

# ---- M. the dashboard's page names a node the cluster named, its slots, and what the cluster said
PD="$(dirname "$BIN")/pagedump"
if [ -x "$PD" ]; then
	n=$("$PD" | grep -o -e "named by the cluster" -e "slots '+esc(q.slots)" -e "Redis Cluster: '+esc(u.redis_cluster.said)" | sort -u | wc -l)
	[ "$n" -ge 3 ] && ok "M. the page names a cluster's nodes, their slots and what the cluster said" ||
		bad "M. the page: $n of 3 strings"
fi

# ---- N. a -MOVED with no host (Redis 7's unknown endpoint) is the asked node's host.  {n}z is added
# now: L's drain copied every key the replicas held into the collection, {n}a among them
for n in m1 m2 m3 r1 r2 r3; do
	python3 -c 'import json,os,sys; f=sys.argv[2]; d=json.load(open(f)); d["{n}z"]={"type": "string", "value": sys.argv[1]}; json.dump(d, open(f + ".tmp", "w")); os.rename(f + ".tmp", f)' "$n" "$D/$n/data.json"
done
set -- $(slot "{n}z"); NS=$1 NO=$(( ($2 + 1) % 3 ))
case $NO in 0) NN=r1;; 1) NN=m2;; 2) NN=m3;; esac
M0=$(st 0 'u["redis_cluster"]["moved"]')
NC='[u.get(k) for k in ("fetches", "fill_string", "absent", "negative_hits", "shadow_hits", "errors", "fill_failed", "too_big", "wrongtype", "fill_present")] + [(x.rsplit(":", 1)[1], a[x].get("fetches"), a[x].get("flushes")) for x in sorted(a)]'
C0=$(st 0 "$NC")
EMPTYHOST=1 topo promoted "$TS,$WS,$NS" "$TS,$WS"
GOT=$(get 0 "{n}z"); w=$(st 0 'u["redis_cluster"]["moved"]'); C1=$(st 0 "$NC")
AR=$(python3 -c 'import json,urllib.request; m=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())["memory"]; print("used %s free %s of %s" % (m.get("arena_used"), m.get("arena_free"), m.get("arena_total")))' 2>&1 | tail -1)
FF=$(st 0 'u.get("fill_failed")')
[ "$GOT" = "$NN" ] && [ "$w" -gt "$M0" ] && ok "N. -MOVED <slot> :<port> (no host) followed to the node asked's host: {n}z from $NN (arena $AR, fill_failed $FF)" ||
	bad "N. empty-host MOVED: {n}z = '$GOT' (want $NN), moved $M0 -> $w; asked: $(for n in m1 m2 m3 r1 r2 r3; do grep -h '{n}z' "$D/$n/counts" 2>/dev/null | sed "s/^/$n /"; done | tr '\n' ';'); in $NN's data: $(grep -c '{n}z' "$D/$NN/data.json"); counters before $C0 after $C1; arena $AR; log: $(cat "$D"/n*.log | grep -i 'arena\|ceiling\|no room' | tail -2 | cut -c1-160 | tr '\n' '|')"
topo promoted "$TS,$WS,$NS"

# ---- O. a master down: writes to its shard refused fast (backed off), the other shards' still written
DOWN=m3 topo promoted "$TS,$WS,$NS"
KS3=$(for i in $(seq 400 460); do set -- $(slot "w$i"); [ "$2" = 2 ] && echo "w$i"; done | head -6)
KS2=$(for i in $(seq 400 460); do set -- $(slot "w$i"); [ "$2" = 1 ] && echo "w$i"; done | head -3)
sleep 0.5
C0=$(grep -c '^DOWN' "$D/m3/counts" 2>/dev/null)
r3=$( { echo "SELECT wt"; for k in $KS3; do echo "SET $k x"; done; } | python3 - $RESP <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
lines = [l.split() for l in sys.stdin.read().splitlines()]
s.sendall(b"".join(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x.encode()) for x in a) for a in lines))
out = [f.readline().decode().strip()[:12] for _ in lines]
print(" ".join(sorted(set(out[1:]))))
PY
)
C1=$(grep -c '^DOWN' "$D/m3/counts" 2>/dev/null)
r2=$(for k in $KS2; do run wt SET "$k" y; done | sort -u | tr '\n' ' ')
n=$((C1 - C0))
[ "$n" -le 3 ] && [ "$r2" = "+OK " ] && ok "O. a master down: its shard's 6 pipelined writes refused ('$r3') with $n dial(s) to it, not one a write; the other shards' written" ||
	bad "O. master down: $n dials for 6 writes ('$r3'); another shard's writes: '$r2'"
topo promoted "$TS,$WS,$NS"

# ---- P. a shard serving its slots in more than 64 ranges: every slot mapped
R0=$(st 0 'u["redis_cluster"]["rounds"]')
topo promoted "$TS,$WS,$NS,6000:6200:2"         # 100 single slots: shard 2 in 100+ ranges
mapped "$((R0 + 1))"
w=$(st 0 'u["redis_cluster"]["said"]')
case "$w" in *"16384 of 16384"*) ok "P. a shard serving its slots in 100+ ranges: every slot mapped";; *) bad "P. fragmented slots: $w";; esac
topo promoted "$TS,$WS,$NS"

# ---- Q. a seed named localhost: the node the cluster calls 127.0.0.1, listed once
k=0; while [ $k -lt 50 ] && [ "$(st hn '"redis_cluster" in u and "16384 of 16384" in u["redis_cluster"]["said"]')" != True ]; do get hn k1 > /dev/null; sleep 0.2; k=$((k+1)); done
w=$(st hn '"localhost:'$PM2'" in a, "127.0.0.1:'$PM2'" in a, a.get("localhost:'$PM2'", {}).get("shard"), len(a)')
[ "$w" = "(True, False, 1, 5)" ] && ok "Q. a seed named localhost is the node the cluster calls 127.0.0.1:$PM2 - listed once, mapped to its shard" ||
	bad "Q. a seed named localhost: (seed listed, 127.0.0.1:$PM2 listed too, its shard, upstreams) = $w"
echo "upclustertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
