#!/bin/sh
# respmapfleettest.sh - S432 (0.7.1): the RESP door's DB-index map, changed on
# a running fleet: one serving `resp_collections = 0, 1:prod-redis, 2:th`
# moves DB 0 onto prod-redis without a restart.
#   - three eager members with RESP doors and that list; DB 0 holds "zero",
#     DB 1 (prod-redis) holds "pr" and a second key;
#   - one RESP connection held open on EACH member, never re-selecting:
#     `perfcli map 0 prod-redis` on node 1, and each connection's next GET
#     answers prod-redis's value (no reconnect, every member);
#   - maps / stats resp.map on every member: index 0 -> prod-redis, runtime;
#     INFO keyspace reports prod-redis as db0; the audit line names the
#     client on node 1 and "by the fleet" on the others;
#   - refused: without -E; a collection that does not exist; index 64;
#     the words swapped (perfcli refuses before sending); an unmap of an
#     index only the config maps;
#   - a reload with resp_collections changed is refused with the
#     `perfcli map` hint; -C reports the runtime map;
#   - the whole fleet restarted: the map holds (the respmap file);
#   - node 3 stopped, `unmap 0` on node 1, node 3 started with its stale
#     map: the fleet's tombstone (newer) brings it back to the config's
#     word within a few beats;
#   - a fourth member that does not take the map (the test flag
#     PERFCACHED_TEST_NO_RESPMAP, as a build before 0.7.1): map is refused
#     naming it; once it has left, map goes through again;
#   - a standalone node (no [cluster]): with allow_create = no map is
#     refused naming the setting; with it on: map, restart, unmap - with no
#     peer to resync from, only the clock resumed above the persisted map
#     lets the unmap outrank it.
# Fail-first: a daemon before S432 answers method-not-found to map.
# Usage: test/respmapfleettest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
D=$(mktemp -d /var/tmp/pcrmf.XXXXXX)
P1ID= P2ID= P3ID= P4ID= P5ID=
trap 'for p in $P1ID $P2ID $P3ID $P4ID $P5ID; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 15511 15512 15513 15514 15515 15521 15522 15523 15524 15525 15531 15532 15533 15534 15535; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "respmapfleettest: port $p already bound" >&2; exit 1; }
done
node() { # node <n> [no-respmap] - start member n
	mkdir -p "$D/state$1"
	[ -f "$D/n$1.conf" ] || cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rmf-client-secret
cluster = rmf-cluster-secret
enable = rmf-enable
[listen]
tcp = 127.0.87.$1:1551$1
http = 127.0.87.$1:1552$1
resp = 127.0.87.$1:1553$1
plaintext = loopback
resp_collections = 0, 1:prod-redis, 2:th
[cluster]
multicast = 239.255.87.32:15590
advertise = 127.0.87.$1
mode = eager
collections = 0, prod-redis, th
[collection 0]
buckets_log2 = 10
[collection prod-redis]
buckets_log2 = 10
[collection th]
buckets_log2 = 10
C
	chmod 600 "$D/n$1.conf"
	n0=$(grep -c "perfcached ready" "$D/n$1.log" 2>/dev/null || echo 0)
	if [ -n "${2:-}" ]; then PERFCACHED_TEST_NO_RESPMAP=1 "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & fi
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && [ "$(grep -c "perfcached ready" "$D/n$1.log")" -le "$n0" ]; do sleep 0.1; i=$((i+1)); done
	[ "$(grep -c "perfcached ready" "$D/n$1.log")" -gt "$n0" ] || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
halt() { eval "hp=\$P${1}ID"; [ -n "$hp" ] && { kill "$hp" 2>/dev/null; wait "$hp" 2>/dev/null; }; eval "P${1}ID="; }
st() { # st <n> <python expr over s (the node's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.87.%s:1552%s/stats" % (sys.argv[1], sys.argv[1]), timeout=5))
print(eval(sys.argv[2]))
PY
}
nv() { # nv <n> <method> [json params] - a native verb, privileged; the result or the error's words
	python3 - "$@" <<'PY' 2>&1
import json, socket, sys
import pcnative
n = sys.argv[1]
f = pcnative.wrap(socket.create_connection(("127.0.87.%s" % n, 15510 + int(n)), 10))
pcnative.call(f, "enable", secret="rmf-enable")
p = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
r = pcnative.call(f, sys.argv[2], **p)
print(json.dumps(r.get("result", r.get("error", {}).get("message")), sort_keys=True))
PY
}
rs() { # rs <n> <db|-> <cmd...> - one RESP command on a fresh connection (SELECT db first)
	python3 - "$@" <<'PY'
import socket, sys
n, db, args = sys.argv[1], sys.argv[2], sys.argv[3:]
s = socket.create_connection(("127.0.87.%s" % n, 15530 + int(n)), 10); f = s.makefile("rwb")
def cmd(a):
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()); f.flush()
    l = f.readline().decode().strip()
    if l.startswith("$") and l != "$-1":
        k = int(l[1:]); b = b""
        while len(b) < k + 2: b += f.read(k + 2 - len(b))
        return b[:k].decode().replace("\r\n", " | ").strip()
    return l
if db != "-":
    cmd(["SELECT", db])
print(cmd(args))
PY
}
mapsof() { st "$1" "sorted((e['index'], e['col'], e['source']) for e in s['resp']['map'])"; }
waitfor() { i=0; while [ $i -lt $(( $1 * 5 )) ] && ! eval "$2"; do sleep 0.2; i=$((i+1)); done; eval "$2"; }
ready() { for n in "$@"; do waitfor 30 '[ "$(st '$n' "s[\"state\"]")" = ready ]' || bad "node $n did not become ready ($(st $n "s['state']"))"; done; }
T=$(cd "$(dirname "$0")" && pwd); PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

node 1; node 2; node 3
waitfor 20 '[ "$(st 1 "s[\"cluster\"][\"peers_up\"]")" = 2 ] && [ "$(st 1 "s[\"state\"]")" = ready ] && [ "$(st 2 "s[\"state\"]")" = ready ] && [ "$(st 3 "s[\"state\"]")" = ready ]' && ok "three members up and ready" || bad "members: peers_up $(st 1 "s['cluster']['peers_up']")"
sleep 1
# ---- 0. the config's map ----------------------------------------------------------------------------
rs 1 0 SET k zero >/dev/null; rs 1 1 SET k pr >/dev/null; rs 1 1 SET k2 pr2 >/dev/null; rs 1 2 SET t th >/dev/null
waitfor 10 '[ "$(rs 3 0 GET k)" = zero ] && [ "$(rs 3 1 GET k)" = pr ] && [ "$(rs 3 1 GET k2)" = pr2 ]' && ok "0. DB 0 holds 'zero', DB 1 (prod-redis) 'pr' - on node 3 too (eager)" || bad "0. node 3: db0 k=$(rs 3 0 GET k) db1 k=$(rs 3 1 GET k)"
r=$(mapsof 2)
[ "$r" = "[(0, '0', 'config'), (1, 'prod-redis', 'config'), (2, 'th', 'config')]" ] && [ "$(st 2 "s['resp']['map_overrides']")" = 0 ] && ok "0. resp.map is the config's: $r" || bad "0. resp.map $r"
# ---- 1. map 0 prod-redis while a connection on each member stays open -------------------------------
cat > "$D/held.py" <<'PY'
import socket, subprocess, sys, time
cli = sys.argv[1]
conns = {}
def cmd(f, a):
    f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()); f.flush()
    l = f.readline().decode().strip()
    if l.startswith("$") and l != "$-1":
        k = int(l[1:]); b = b""
        while len(b) < k + 2: b += f.read(k + 2 - len(b))
        return b[:k].decode()
    return l
for n in (1, 2, 3):
    s = socket.create_connection(("127.0.87.%d" % n, 15530 + n), 10)
    conns[n] = (s, s.makefile("rwb"))
before = {n: cmd(f, ["GET", "k"]) for n, (s, f) in conns.items()}
out = subprocess.run([cli, "-q", "-h", "127.0.87.1", "-p", "15511", "-E", "rmf-enable", "map", "0", "prod-redis"],
                     capture_output=True, text=True, timeout=20)
after = {}
t0 = time.time()
while time.time() - t0 < 10:
    after = {n: cmd(f, ["GET", "k"]) for n, (s, f) in conns.items()}
    if all(v == "pr" for v in after.values()):
        break
    time.sleep(0.2)
print("before=%s" % ",".join(before[n] for n in (1, 2, 3)))
print("after=%s" % ",".join(after[n] for n in (1, 2, 3)))
print("secs=%.1f" % (time.time() - t0))
print("cli=%s" % (out.stdout + out.stderr).replace("\n", " "))
PY
python3 "$D/held.py" "$CLI" > "$D/held.out" 2>&1
grep -q '"mapped": *true' "$D/held.out" && ok "1. perfcli map 0 prod-redis on node 1: $(sed -n 's/^cli=//p' "$D/held.out" | cut -c1-140)" || bad "1. perfcli map: $(cat "$D/held.out" | tr '\n' '|' | cut -c1-300)"
[ "$(sed -n 's/^before=//p' "$D/held.out")" = "zero,zero,zero" ] && [ "$(sed -n 's/^after=//p' "$D/held.out")" = "pr,pr,pr" ] &&
	ok "1. the SAME open connection on each member read 'zero' before and 'pr' after - no reconnect, no SELECT ($(sed -n 's/^secs=//p' "$D/held.out") s)" || bad "1. held connections: $(tr '\n' ' ' < "$D/held.out" | cut -c1-200)"
for n in 1 2 3; do
	r=$(mapsof $n)
	[ "$r" = "[(0, 'prod-redis', 'runtime'), (1, 'prod-redis', 'config'), (2, 'th', 'config')]" ] && [ "$(st $n "s['resp']['map_overrides']")" = 1 ] || { bad "1. node $n resp.map $r"; continue; }
	[ $n = 3 ] && ok "1. every member's resp.map: 0 -> prod-redis (runtime), 1 and 2 as the config says"
done
r=$(rs 2 - INFO keyspace)
case "$r" in *"db0:keys=2,"*"db2:keys=1,"*) case "$r" in *"db1:"*) bad "1. INFO keyspace still names db1: $r" ;; *) ok "1. INFO keyspace follows the map: prod-redis is db0 (2 keys), th db2, collection '0' unreachable" ;; esac ;; *) bad "1. INFO keyspace: $r" ;; esac
grep -q "resp: DB index 0 mapped to collection 'prod-redis' by .* (was '0')" "$D/n1.log" && ok "1. the audit line on node 1 names the client and old -> new: $(grep -h "DB index 0 mapped" "$D/n1.log" | head -1 | sed 's/.*resp: //' | cut -c1-120)" || bad "1. n1.log: $(grep -h 'resp:' "$D/n1.log" | tail -1 | cut -c1-140)"
grep -q "resp: DB index 0 mapped to collection 'prod-redis' by the fleet (was '0')" "$D/n2.log" && grep -q "by the fleet (was '0')" "$D/n3.log" && ok "1. nodes 2 and 3 log the map as the fleet's, old -> new" || bad "1. n2/n3 logs: $(grep -h 'resp:' "$D/n2.log" "$D/n3.log" | tail -2 | tr '\n' '|' | cut -c1-200)"
[ "$(rs 3 0 GET k2)" = pr2 ] && [ "$(rs 3 1 GET k2)" = pr2 ] && ok "1. a fresh connection: DB 0 and DB 1 are the same collection now" || bad "1. db0 k2 $(rs 3 0 GET k2), db1 k2 $(rs 3 1 GET k2)"
# ---- 2. refusals ------------------------------------------------------------------------------------
r=$("$CLI" -q -h 127.0.87.2 -p 15512 map 3 th 2>&1)
echo "$r" | grep -q -i "privilege\|enable" && ok "2. map without -E is refused: $(echo "$r" | head -1 | cut -c1-100)" || bad "2. unprivileged map: $r"
r=$(nv 2 map '{"index":3,"col":"no-such"}'); echo "$r" | grep -q "no such collection" && ok "2. a collection that does not exist: $r" || bad "2. no-such: $r"
r=$(nv 2 map '{"index":64,"col":"th"}'); echo "$r" | grep -q "0..63" && ok "2. index 64: $r" || bad "2. index 64: $r"
r=$("$CLI" -q -h 127.0.87.2 -p 15512 -E rmf-enable map prod-redis 0 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$r" | grep -q "bad arguments for 'map'" && [ "$(st 2 "s['resp']['map_overrides']")" = 1 ] && ok "2. the words swapped (map prod-redis 0): perfcli refuses before sending" || bad "2. swapped: rc $rc $r"
r=$(nv 2 unmap '{"index":2}'); echo "$r" | grep -q "not mapped at runtime" && ok "2. unmap of an index only the config maps: $r" || bad "2. unmap 2: $r"
# ---- 3. reload and -C --------------------------------------------------------------------------------
cp "$D/n1.conf" "$D/n1.conf.orig"
sed -i 's/^resp_collections = .*/resp_collections = 0:prod-redis, 1:prod-redis, 2:th/' "$D/n1.conf"
r=$("$CLI" -q -h 127.0.87.1 -p 15511 -E rmf-enable reload 2>&1)
echo "$r" | grep -q "resp_collections changed and cannot be reloaded" && echo "$r" | grep -q "perfcli map <index> <collection>" && ok "3. a reload with resp_collections changed is refused with the perfcli map hint" || bad "3. reload: $r"
cp "$D/n1.conf.orig" "$D/n1.conf"
r=$("$BIN" -C -f "$D/n1.conf" 2>&1)
echo "$r" | grep -q "resp: 1 DB index(es) mapped at runtime ($D/state1/respmap) outrank resp_collections" && ok "3. -C says a runtime map outranks resp_collections" || bad "3. -C: $(echo "$r" | grep -i resp | head -2)"
# ---- 4. the whole fleet restarted: the map holds ----------------------------------------------------
head -1 "$D/state2/respmap" | grep -q "^perfcached-respmap 1" && grep -q "^map 0 [0-9]* 1 prod-redis" "$D/state2/respmap" && ok "4. node 2's respmap file holds it" || bad "4. respmap: $(cat "$D/state2/respmap" 2>&1 | tr '\n' '|')"
halt 1; halt 2; halt 3
node 1; node 2; node 3
waitfor 20 '[ "$(st 1 "s[\"cluster\"][\"peers_up\"]")" = 2 ]' || bad "4. the fleet did not come back"
ready 1 2 3
for n in 1 2 3; do [ "$(mapsof $n)" = "[(0, 'prod-redis', 'runtime'), (1, 'prod-redis', 'config'), (2, 'th', 'config')]" ] || bad "4. node $n after the restart: $(mapsof $n)"; done
# no WAL or snapshot here: the records went with the fleet, the map did not.
# Seed them again over the native door - collection '0' has no RESP index
# while DB 0 is mapped away from it.
"$CLI" -q -h 127.0.87.1 -p 15511 set 0 k zero >/dev/null 2>&1; "$CLI" -q -h 127.0.87.1 -p 15511 set prod-redis k pr >/dev/null 2>&1
waitfor 10 '[ "$(rs 3 0 GET k)" = pr ]'
[ "$(rs 1 0 GET k)" = pr ] && [ "$(rs 3 0 GET k)" = pr ] && grep -q "1 DB index(es) mapped at runtime" "$D/n2.log" && ok "4. after a full restart DB 0 is still prod-redis everywhere, and the start says so" || bad "4. db0 k: $(rs 1 0 GET k)/$(rs 3 0 GET k)"
waitfor 10 '[ "$(st 3 "[c[\"entries\"] for c in s[\"collections\"] if c[\"name\"]==\"0\"][0]")" = 1 ]' || bad "4. collection 0 did not reach node 3"
# ---- 5. a member that missed the unmap -----------------------------------------------------------------
halt 3
sleep 1
r=$(nv 1 unmap '{"index":0}'); echo "$r" | grep -q '"unmapped": true' && ok "5. unmap 0 on node 1 while node 3 is down: $r" || bad "5. unmap: $r"
[ "$(rs 2 0 GET k)" = zero ] && ok "5. node 2: DB 0 is collection '0' again" || bad "5. node 2 db0 k: $(rs 2 0 GET k)"
node 3
ready 3
waitfor 15 '[ "$(mapsof 3)" = "[(0, '"'0'"', '"'config'"'), (1, '"'prod-redis'"', '"'config'"'), (2, '"'th'"', '"'config'"')]" ]' && [ "$(rs 3 0 GET k)" = zero ] &&
	ok "5. node 3 came back with its stale map and the fleet's newer unmap took it back to the config's word" || bad "5. node 3: $(mapsof 3), db0 k $(rs 3 0 GET k)"
grep -q "resp: DB index 0 unmapped by the fleet (was 'prod-redis')" "$D/n3.log" && ok "5. node 3 logs the unmap as the fleet's" || bad "5. n3.log: $(grep -h 'resp:' "$D/n3.log" | tail -2 | tr '\n' '|' | cut -c1-200)"
# ---- 6. a member that does not take the map ---------------------------------------------------------------
node 4 no-respmap
waitfor 20 '[ "$(st 1 "s[\"cluster\"][\"peers_up\"]")" = 3 ]' || bad "6. node 4 did not join"
ready 4
grep -q "PERFCACHED_TEST_NO_RESPMAP" "$D/n4.log" && ok "6. node 4 runs with the test flag (logged)" || bad "6. no flag line in n4.log"
sleep 2
r=$(nv 1 map '{"index":0,"col":"prod-redis"}')
echo "$r" | grep -q "every member must run 0.7.1 or later to map a RESP index at runtime - node [0-9]* is older" && ok "6. map refused while node 4 is a member: $r" || bad "6. map with node 4: $r"
[ "$(st 1 "s['resp']['map_overrides']")" = 0 ] && [ "$(rs 1 0 GET k)" = zero ] && ok "6. nothing changed" || bad "6. overrides $(st 1 "s['resp']['map_overrides']")"
halt 4
waitfor 15 '[ "$(st 1 "s[\"cluster\"][\"peers_up\"]")" = 2 ]' || bad "6. node 4 did not leave"
r=$(nv 1 map '{"index":0,"col":"prod-redis"}')
echo "$r" | grep -q '"mapped": true' && waitfor 5 '[ "$(rs 3 0 GET k)" = pr ]' && ok "6. once node 4 has left, map goes through and reaches node 3" || bad "6. map after node 4 left: $r; node 3 db0 k $(rs 3 0 GET k)"
r=$(nv 1 unmap '{"index":0}'); echo "$r" | grep -q '"unmapped": true' && waitfor 5 '[ "$(rs 3 0 GET k)" = zero ]' && ok "6. unmap back: DB 0 is collection '0' on node 3 again" || bad "6. unmap back: $r"
# ---- 7. a standalone node: the unmap after a restart outranks the map ------------------------------------
mkdir -p "$D/state5"
cat > "$D/n5.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state5
allow_create = no
[memory]
arena_mb = 32
[secrets]
client = rmf-client-secret
enable = rmf-enable
[listen]
tcp = 127.0.87.5:15515
http = 127.0.87.5:15525
resp = 127.0.87.5:15535
plaintext = loopback
resp_collections = 0, 1:prod-redis
[collection 0]
buckets_log2 = 8
[collection prod-redis]
buckets_log2 = 8
C
chmod 600 "$D/n5.conf"
solo() { n0=$(grep -c "perfcached ready" "$D/n5.log" 2>/dev/null || echo 0); "$BIN" -f "$D/n5.conf" >> "$D/n5.log" 2>&1 & P5ID=$!
	i=0; while [ $i -lt 100 ] && [ "$(grep -c "perfcached ready" "$D/n5.log")" -le "$n0" ]; do sleep 0.1; i=$((i+1)); done; }
solo
r=$(nv 5 map '{"index":0,"col":"prod-redis"}')
echo "$r" | grep -q "allow_create" && ok "7. with allow_create = no, map is refused naming the setting: $r" || bad "7. allow_create = no: $r"
halt 5; sed -i 's/^allow_create = no/allow_create = yes/' "$D/n5.conf"; solo
r=$(nv 5 map '{"index":0,"col":"prod-redis"}'); echo "$r" | grep -q '"mapped": true' || bad "7. standalone map: $r"
halt 5; solo
r=$(nv 5 unmap '{"index":0}')
echo "$r" | grep -q '"unmapped": true' && [ "$(st 5 "sorted((e['index'], e['source']) for e in s['resp']['map'])")" = "[(0, 'config'), (1, 'config')]" ] &&
	ok "7. standalone: map, restart, unmap - the unmap outranks the persisted map (the clock resumed above it)" || bad "7. standalone unmap after a restart: $r"
halt 5
grep -q -E 'CRIT' "$D"/n[123].log && bad "a node logged CRIT: $(grep -h -m1 CRIT "$D"/n[123].log | cut -c1-120)" || ok "no CRIT in the members' logs"
echo "respmapfleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
