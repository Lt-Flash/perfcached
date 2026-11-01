#!/bin/sh
# upthroughshardtest.sh - DESIGN 12nt, TC-1 (0.7.1): a read-through
# collection in SHARD mode fetches at the key's owner, and nowhere else.
#   - three shard members, collection 0 read-through on the fakes (M, R1,
#     R2 of upwritethroughtest), 30 keys seeded straight into the master;
#   - every key reads through node 1 with its value: a key node 1 does not
#     own is pulled from the owner, which fetches it from a replica and
#     keeps the copy (cluster.pull_fetching on the owners);
#   - the copies sit on the owners only: the fleet's shadow_entries sum to
#     30 and no node holds them all; the fakes were fetched 30 times;
#   - the same keys through node 2: found, and not fetched again;
#   - a change on the master reaches the owner's copy (tracking): the next
#     read through node 3 is the new value, one more fetch;
#   - an absent key reads as absent through a non-owner, one fetch (the
#     owner's negative marker).
#   TC-2 (the write at the owner):
#   - a write through node 2 for a key another node owns lands on that node
#     only, and the fake master got the SET exactly once (cluster.fwd_through
#     = fwd_through_acked on the owner);
#   - SET NX through node 2: the master decides - stored once, the second one
#     refused and stored nowhere; SET XX on an absent key: nothing anywhere;
#   - a delete through node 2 reaches the master and empties the owner;
#   - the RESP forms (SET, SET NX, DEL, INCRBY, EXPIRE) through node 2 do the same;
#   TC-4: the drain sweep on every node takes over the owner's share only.
# Before 12nt the node a client read on fetched and kept its own copy, so
# three nodes held three copies and the fakes were fetched three times.
# Usage: test/upthroughshardtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcupsh.XXXXXX)
PM=19901 P1=19902 P2=19903
FM= F1= F2= P1ID= P2ID= P3ID=
trap 'for p in $P1ID $P2ID $P3ID $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $P2 19911 19912 19913 19921 19922 19923 19931 19932 19933; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upthroughshardtest: port $p already bound" >&2; exit 1; }
done
ctl() { echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"; }
for n in m 1 2; do mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"; done
ctl m '{"role": "master", "accept_writes": true, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
rawm() { python3 - $PM "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
print(s.makefile("rb").readline().decode("latin-1").strip())
PY
}
node() { # node <n>
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = upsh-client-secret
cluster = upsh-cluster-secret
enable = upsh-enable
[listen]
tcp = 127.0.0.1:1991$1
resp = 127.0.0.1:1993$1
http = 127.0.0.1:1992$1
plaintext = loopback
[cluster]
multicast = 239.255.78.24:19894
advertise = 127.0.84.$1
mode = shard
collections = 0
pull_timeout_ms = 200
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 5000
C
	chmod 600 "$D/n$1.conf"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
st() { # st <n> <python expr over s (the node's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:1992%s/stats" % sys.argv[1], timeout=5))
print(eval(sys.argv[2]))
PY
}
col0() { st "$1" "[c for c in s['collections'] if c['name']=='0'][0]$2"; }
nget() { # nget <n> <key> -> the value, or None
	python3 - "$1" "$2" <<'PY' 2>&1
import socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19910 + int(sys.argv[1])), 10))
r = pcnative.call(f, "get", col="0", key=sys.argv[2])
e = r.get("error")
print("ERR " + str(e) if e else (r["result"].get("value") if r["result"].get("found") else None))
PY
}
for k in $(seq 1 30); do rawm SET "k:$k" "v$k" > /dev/null; done
[ "$(rawm GET k:30)" = '$3' ] && ok "30 keys seeded on the fake master (replicated to R1, R2)" || bad "seed: $(rawm GET k:30)"
node 1; node 2; node 3
i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']")" != 2 ]; do sleep 0.2; i=$((i+1)); done
[ "$(st 1 "s['cluster']['peers_up']")" = 2 ] && ok "three shard members up" || bad "peers_up on node 1: $(st 1 "s['cluster']['peers_up']")"
for n in 1 2 3; do i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done; done
sleep 1

# ---- 1. every key reads through node 1 with its value ----------------------------------
miss=0; for k in $(seq 1 30); do [ "$(nget 1 "k:$k")" = "v$k" ] || { miss=$((miss+1)); [ $miss -le 2 ] && echo "   k:$k via node 1: $(nget 1 "k:$k")"; }; done
[ $miss -eq 0 ] && ok "1. all 30 keys read through node 1 with their values" || bad "1. $miss of 30 wrong or absent through node 1"
f1=$(col0 1 "['upstream']['fetches']"); f2=$(col0 2 "['upstream']['fetches']"); f3=$(col0 3 "['upstream']['fetches']")
s1=$(col0 1 "['upstream']['shadow_entries']"); s2=$(col0 2 "['upstream']['shadow_entries']"); s3=$(col0 3 "['upstream']['shadow_entries']")
pf2=$(st 2 "s['cluster']['pull_fetching']"); pf3=$(st 3 "s['cluster']['pull_fetching']"); pd2=$(st 2 "s['cluster']['pull_fetched']"); pd3=$(st 3 "s['cluster']['pull_fetched']")
[ $((f1 + f2 + f3)) -eq 30 ] && ok "1. the fakes were fetched 30 times in all (node 1: $f1, node 2: $f2, node 3: $f3) - once per key, at its owner" || bad "1. fetches $f1 + $f2 + $f3 (30 expected)"
[ $((s1 + s2 + s3)) -eq 30 ] && [ "$s1" -lt 30 ] && [ "$s2" -lt 30 ] && [ "$s3" -lt 30 ] && ok "1. the copies sit on the owners: shadow_entries $s1 / $s2 / $s3, 30 in all, no node holds them all" || bad "1. shadow_entries $s1 / $s2 / $s3"
[ $((pf2 + pf3)) -gt 0 ] && [ $((pf2 + pf3)) -eq $((pd2 + pd3)) ] && [ $((pf2 + pf3)) -eq $((f2 + f3)) ] && ok "1. the owners fetched for node 1's pulls: pull_fetching $pf2 + $pf3 = pull_fetched $pd2 + $pd3 = their fetches" || bad "1. pull_fetching $pf2/$pf3, pull_fetched $pd2/$pd3, fetches $f2/$f3"

# ---- 2. the same keys through node 2: found, not fetched again ----------------------------
miss=0; for k in $(seq 1 30); do [ "$(nget 2 "k:$k")" = "v$k" ] || miss=$((miss+1)); done
f1b=$(col0 1 "['upstream']['fetches']"); f2b=$(col0 2 "['upstream']['fetches']"); f3b=$(col0 3 "['upstream']['fetches']")
s1b=$(col0 1 "['upstream']['shadow_entries']"); s2b=$(col0 2 "['upstream']['shadow_entries']"); s3b=$(col0 3 "['upstream']['shadow_entries']")
[ $miss -eq 0 ] && [ $((f1b + f2b + f3b)) -eq 30 ] && [ $((s1b + s2b + s3b)) -eq 30 ] && ok "2. the same 30 keys through node 2: all found, no second fetch, still 30 copies" || bad "2. miss $miss, fetches $f1b/$f2b/$f3b, copies $s1b/$s2b/$s3b"

# ---- 3. a change on the master reaches the owner's copy ---------------------------------
rawm SET k:5 changed > /dev/null
sleep 0.5
[ "$(nget 3 k:5)" = changed ] && ok "3. k:5 changed on the master: read through node 3 is the new value (the owner's copy was invalidated and fetched again)" || bad "3. k:5 via node 3: $(nget 3 k:5)"
f1c=$(col0 1 "['upstream']['fetches']"); f2c=$(col0 2 "['upstream']['fetches']"); f3c=$(col0 3 "['upstream']['fetches']")
[ $((f1c + f2c + f3c)) -eq 31 ] && ok "3. one more fetch in all ($((f1c + f2c + f3c)))" || bad "3. fetches $f1c/$f2c/$f3c (31 expected)"

# ---- 4. an absent key through a non-owner ------------------------------------------------
# an absent key is asked of every upstream before it is absent (one fetch per upstream, three
# here); the owner's negative marker then answers the next pull without a fetch
r=$(nget 1 k:nope)
f1d=$(col0 1 "['upstream']['fetches']"); f2d=$(col0 2 "['upstream']['fetches']"); f3d=$(col0 3 "['upstream']['fetches']")
r2=$(nget 2 k:nope)
f1e=$(col0 1 "['upstream']['fetches']"); f2e=$(col0 2 "['upstream']['fetches']"); f3e=$(col0 3 "['upstream']['fetches']")
nh=$(( $(col0 1 "['upstream']['negative_hits']") + $(col0 2 "['upstream']['negative_hits']") + $(col0 3 "['upstream']['negative_hits']") ))
[ "$r" = None ] && [ "$r2" = None ] && ok "4. an absent key reads as absent through nodes 1 and 2" || bad "4. k:nope: $r / $r2"
[ $((f1d + f2d + f3d)) -gt 31 ] && [ $((f1d + f2d + f3d)) -le 34 ] && ok "4. the first read fetched the absent key once per upstream ($((f1d + f2d + f3d - 31)) fetches, at the owner)" || bad "4. fetches after the first absent read $f1d/$f2d/$f3d (32..34 expected)"
[ $((f1e + f2e + f3e)) -eq $((f1d + f2d + f3d)) ] && [ "$nh" -ge 1 ] && ok "4. the second read through node 2 fetched nothing: the owner's negative marker answered the pull (negative_hits $nh)" || bad "4. fetches after the second read $f1e/$f2e/$f3e (was $f1d/$f2d/$f3d), negative_hits $nh"

# ---- TC-2: the write at the owner --------------------------------------------------------
# nset <n> <verb> <key> <value> [ttl] - one native write, its result JSON
nset() {
	python3 - "$@" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19910 + int(sys.argv[1])), 10))
kw = dict(col="0", key=sys.argv[3], value=sys.argv[4])
if len(sys.argv) > 5: kw["ttl"] = int(sys.argv[5])
r = pcnative.call(f, sys.argv[2], **kw)
print(json.dumps(r.get("result") if "result" in r else r, sort_keys=True))
PY
}
ndel() { # ndel <n> <key>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19910 + int(sys.argv[1])), 10))
print(json.dumps(pcnative.call(f, "del", col="0", key=sys.argv[2]).get("result"), sort_keys=True))
PY
}
mdata() { # mdata <key> <field> - the fake master's stored entry field, or None
	python3 -c 'import json,sys
d=json.load(open(sys.argv[1]+"/data.json")); e=d.get(sys.argv[2])
print(None if e is None else e.get(sys.argv[3]))' "$D/upm" "$1" "$2" 2>/dev/null
}
holders() { # the entries of collection 0 on nodes 1 2 3 (shard: a key sits on its owner only)
	python3 - <<'PY' 2>&1
import json, urllib.request
h = []
for n in (1, 2, 3):
    s = json.load(urllib.request.urlopen("http://127.0.0.1:1992%d/stats" % n, timeout=5))
    h.append([c for c in s["collections"] if c["name"] == "0"][0]["entries"])
print(" ".join(str(x) for x in h))
PY
}
# a key node 2 does NOT own: written through node 2, its owner runs it through (the owner's
# fwd_through grows by one; the record itself is dropped and refetched by the write's own echo
# within milliseconds, so entries cannot tell the owner)
fwt() { echo "$(col0 1 "['upstream']['fwd_through']") $(col0 2 "['upstream']['fwd_through']") $(col0 3 "['upstream']['fwd_through']")"; }
e0=$(fwt); OWN=; WK=
for cand in $(seq 1 40); do
	r=$(nset 2 set "w:$cand" "first")
	echo "$r" | grep -q '"stored": true' || { echo "   set w:$cand via node 2: $r"; continue; }
	e1=$(fwt)
	n=$(python3 -c 'import sys
a=[int(x) for x in sys.argv[1].split()]; b=[int(x) for x in sys.argv[2].split()]
d=[i+1 for i in range(3) if b[i]>a[i]]; print(d[0] if len(d)==1 else 0)' "$e0" "$e1")
	e0=$e1
	[ "$n" != 0 ] && [ "$n" != 2 ] && { OWN=$n; WK="w:$cand"; break; }
done
[ -n "$WK" ] && ok "5. $WK written through node 2 ran through at its owner, node $OWN (fwd_through $e1)" || { bad "5. no key written through node 2 was forwarded to another node (fwd_through $e1)"; WK=w:none; OWN=3; }
e1=$(holders)
nw=$(grep -c "^SET $WK first" "$D/upm/writes" 2>/dev/null)
[ "$nw" = 1 ] && [ "$(mdata $WK value)" = first ] && ok "5. the master got that SET exactly once and holds the value" || bad "5. SET $WK on the master: $nw time(s), value $(mdata $WK value)"
ft=$(st $OWN "s['cluster']['fwd_through']"); fa=$(st $OWN "s['cluster']['fwd_through_acked']"); cf=$(col0 $OWN "['upstream']['fwd_through']")
[ "$ft" -ge 1 ] && [ "$ft" = "$fa" ] && [ "$cf" = "$ft" ] && ok "5. node $OWN ran the forwarded write through: cluster.fwd_through $ft = fwd_through_acked $fa = collection fwd_through $cf" || bad "5. owner $OWN counters: fwd_through $ft, acked $fa, collection $cf"
[ "$(nget 1 $WK)" = first ] && [ "$(nget 2 $WK)" = first ] && ok "5. $WK reads back through nodes 1 and 2" || bad "5. $WK via node 1: $(nget 1 $WK), node 2: $(nget 2 $WK)"

# 6. SET NX through a non-owner: the master decides, the owner stores, nowhere else
r=$(nset 2 setnx "$WK:nx" tokA 60)
h1=$(holders)
echo "$r" | grep -q '"stored": true' && [ "$(mdata $WK:nx value)" = tokA ] && grep -q "^SET $WK:nx tokA NX EX 60$" "$D/upm/writes" && ok "6. SET NX through node 2: stored, the master got 'SET $WK:nx tokA NX EX 60'" || bad "6. setnx: $r, master value $(mdata $WK:nx value), writes: $(grep "$WK:nx" "$D/upm/writes" | tr '\n' '|')"
r2=$(nset 2 setnx "$WK:nx" tokB 60)
h2=$(holders)
echo "$r2" | grep -q '"stored": false' && [ "$(mdata $WK:nx value)" = tokA ] && ok "6. the second SET NX is refused by the master: stored false, tokA kept" || bad "6. second setnx: $r2, master value $(mdata $WK:nx value)"
[ "$h1" = "$h2" ] && [ "$(grep -c "^SET $WK:nx" "$D/upm/writes")" = 2 ] && ok "6. both NX writes reached the master (2 SETs), the refused one stored nowhere (entries $h2 unchanged)" || bad "6. SET $WK:nx on the master $(grep -c "^SET $WK:nx" "$D/upm/writes") time(s), entries $h1 -> $h2"
[ "$(nget 3 $WK:nx)" = tokA ] && ok "6. $WK:nx reads tokA through node 3" || bad "6. $WK:nx via node 3: $(nget 3 $WK:nx)"

# 7. SET XX on an absent key through a non-owner: nil at the master, stored nowhere
r=$(nset 2 setxx "$WK:xx" never)
echo "$r" | grep -q '"stored": false' && [ "$(mdata $WK:xx value)" = None ] && [ "$(nget 1 $WK:xx)" = None ] && ok "7. SET XX on an absent key through node 2: stored false, absent on the master and in the fleet" || bad "7. setxx: $r, master $(mdata $WK:xx value), node 1 $(nget 1 $WK:xx)"

# 8. a delete through a non-owner goes to the master, then the owner's copy is gone
r=$(ndel 2 "$WK")
h3=$(holders)
echo "$r" | grep -q '"deleted": true' && [ "$(mdata $WK value)" = None ] && grep -q "^DEL $WK$" "$D/upm/writes" && ok "8. del $WK through node 2: deleted, the master got DEL, the key is gone there" || bad "8. del: $r, master $(mdata $WK value), writes $(grep "^DEL" "$D/upm/writes" | tr '\n' '|')"
[ "$(nget 1 $WK)" = None ] && [ "$(nget 3 $WK)" = None ] && ok "8. $WK reads as absent through nodes 1 and 3 (entries $h3)" || bad "8. $WK after del: node 1 $(nget 1 $WK), node 3 $(nget 3 $WK)"
r=$(ndel 2 "$WK:gone")
echo "$r" | grep -q '"deleted": false' && ok "8. deleting an absent key through node 2 answers deleted false (the master's 0)" || bad "8. del absent: $r"

# 9. the RESP forms through a non-owner: the same ops, the same forward to the owner
rsp() { # rsp <n> <args...> - one command at node n's RESP door; prints its reply as python repr
	python3 - "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", 19930 + int(sys.argv[1])), 10)
f = s.makefile("rb")
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
def rd():
    l = f.readline()
    t, body = l[:1], l[1:-2]
    if t == b"$":
        n = int(body)
        return None if n < 0 else f.read(n + 2)[:-2].decode("latin-1")
    if t == b":":
        return int(body)
    return (t + body).decode("latin-1")
print(repr(rd()))
PY
}
ft0=$(st $OWN "s['cluster']['fwd_through']")
r1=$(rsp 2 SET "$WK" rv); r2=$(rsp 2 SET "$WK" nope NX); r3=$(rsp 2 DEL "$WK"); r4=$(rsp 2 INCRBY "$WK" 7); r5=$(rsp 2 EXPIRE "$WK" 100)
[ "$r1" = "'+OK'" ] && [ "$r2" = None ] && [ "$r3" = 1 ] && [ "$r4" = 7 ] && [ "$r5" = 1 ] && ok "9. RESP through node 2: SET +OK, SET NX nil, DEL 1, INCRBY 7, EXPIRE 1 - the master's answers" || bad "9. RESP replies: SET $r1, SET NX $r2, DEL $r3, INCRBY $r4, EXPIRE $r5"
grep -q "^SET $WK rv$" "$D/upm/writes" && grep -q "^SET $WK nope NX$" "$D/upm/writes" && grep -q "^INCRBY $WK 7$" "$D/upm/writes" && grep -q "^EXPIRE $WK 100$" "$D/upm/writes" && [ "$(mdata $WK value)" = 7 ] && ok "9. the master got SET, SET NX, DEL, INCRBY and EXPIRE as sent, and holds 7" || bad "9. master writes: $(grep " $WK" "$D/upm/writes" | tail -6 | tr '\n' '|'), value $(mdata $WK value)"
ft1=$(st $OWN "s['cluster']['fwd_through']"); fa1=$(st $OWN "s['cluster']['fwd_through_acked']")
[ "$ft1" = $((ft0 + 5)) ] && [ "$fa1" = "$ft1" ] && ok "9. all five ran through at the owner (node $OWN: fwd_through $ft0 -> $ft1, acked $fa1)" || bad "9. owner $OWN fwd_through $ft0 -> $ft1, acked $fa1 (5 more expected)"
[ "$(nget 1 $WK)" = 7 ] && ok "9. $WK reads 7 through node 1" || bad "9. $WK via node 1: $(nget 1 $WK)"


# ---- 10. the drain sweep takes over each node's share --------------------------------------
drain() { # drain <n> - updrain on node n (privileged), its result
	python3 - "$1" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19910 + int(sys.argv[1])), 10))
pcnative.call(f, "enable", secret="upsh-enable")
r = pcnative.call(f, "updrain", col="0")
print(json.dumps(r.get("result", r.get("error")), sort_keys=True))
PY
}
dst() { col0 "$1" "['upstream']['drain']['$2']"; }
NR=$(python3 -c 'import json,sys,time; d=json.load(open(sys.argv[1])); print(sum(1 for k,e in d.items() if not e.get("exp") or e["exp"] > time.time()*1000))' "$D/upm/data.json")
for n in 1 2 3; do r=$(drain $n); echo "$r" | grep -q -i 'error' && echo "   updrain on node $n: $r"; done
i=0; while [ $i -lt 100 ] && { for n in 1 2 3; do [ "$(dst $n state)" = done ] || exit 1; done; } 2>/dev/null; [ $? -ne 0 ]; do sleep 0.2; i=$((i+1)); done
RD=; TK=; HD=; EL=; for n in 1 2 3; do RD="$RD $(dst $n read)"; TK="$TK $(dst $n taken_over)"; HD="$HD $(dst $n fleet_had)"; EL="$EL $(dst $n elsewhere)"; done
sum() { python3 -c 'import sys; print(sum(int(x) for x in sys.argv[1].split()))' "$1"; }
allv() { python3 -c 'import sys; print(all(int(x) == int(sys.argv[2]) for x in sys.argv[1].split()))' "$1" "$2"; }
[ "$(sum "$RD")" -eq "$NR" ] && ok "10. the drain sweeps read each of the master's $NR keys at its owner alone (read:$RD)" || bad "10. drain read:$RD ($NR keys on the master, 1 x expected)"
[ $(( $(sum "$TK") + $(sum "$HD") )) -eq $((NR * 1)) ] && [ "$(sum "$EL")" -eq $((NR * 2)) ] && ok "10. each key taken (or already held) by its owner alone - taken_over:$TK fleet_had:$HD, the other nodes skipped it (elsewhere:$EL)" || bad "10. taken_over:$TK fleet_had:$HD elsewhere:$EL (want 1 x $NR taken+had, 2 x $NR elsewhere)"
grep -q -E 'ERROR|CRIT' "$D"/n?.log && bad "the nodes logged: $(grep -h -m2 -E 'ERROR|CRIT' "$D"/n?.log | tr '\n' '|')" || ok "no ERROR or CRIT in the nodes' logs"
echo "upthroughshardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
