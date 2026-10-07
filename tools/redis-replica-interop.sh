#!/bin/sh
# redis-replica-interop.sh - read-through against a REAL Redis REPLICA,
# the way a fleet takes a keyspace over (DESIGN 12ix, 12ja).
#
# A throwaway redis:8 master and a replica of it, in two local containers
# sharing one network namespace (podman, or docker), removed on exit -
# never point this at a shared Redis.  Not in CI (no image pulls there):
#
#   tools/redis-replica-interop.sh [./perfcached] [image]
#
#   1  perfcached reads the REPLICA as the GUIDE's read-only user: tracking
#      on, and stats name the replica's run_id as the tracking server
#   2  writes, deletes and expiries made on the MASTER reach perfcached as
#      invalidations FROM THE REPLICA (it applies them from the master's
#      stream): the next read has the change
#   3  the replica restarts: the copies are dropped, tracking comes back
#      on the new server (a new run_id), reads are right
#   4  a round-robin proxy over master and replica: the fetching
#      connection reached the other server, so it is not tracked - a
#      change made on the master is read within upstream_negative_ms
#   5  a user without +info: no tracking at all, reads still work
#   6  MONITOR on both servers: only the handshake and reads; ACL LOG
#      empty on both (the replica's from its restart on: until its users
#      are created again, perfcached's reconnects fail AUTH there), and
#      for the user without +info exactly the refused INFO
set -u
BIN=$(readlink -f "${1:-./perfcached}")
IMG=${2:-docker.io/library/redis:8}
ENG=
for e in podman docker; do command -v $e >/dev/null 2>&1 && { ENG=$e; break; }; done
[ -n "$ENG" ] || { echo "redis-replica-interop: SKIPPED - neither podman nor docker"; exit 0; }
MP=18790; RP=18791; XP=18792; RESP=18795; HTTP=18796; TCP=18797
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcrepl.XXXXXX)
PID= M1= M2= XPID=
cleanup() {
	[ -n "$PID" ] && kill $PID 2>/dev/null
	[ -n "$M1" ] && kill $M1 2>/dev/null
	[ -n "$M2" ] && kill $M2 2>/dev/null
	[ -n "$XPID" ] && kill $XPID 2>/dev/null
	$ENG rm -f s331-rr >/dev/null 2>&1
	$ENG rm -f s331-rm >/dev/null 2>&1
	rm -rf "$D"
}
trap cleanup EXIT INT TERM
$ENG rm -f s331-rr >/dev/null 2>&1
$ENG rm -f s331-rm >/dev/null 2>&1
$ENG run -d --rm --name s331-rm -p 127.0.0.1:$MP:6379 -p 127.0.0.1:$RP:6380 "$IMG" \
	redis-server --save "" --appendonly no > /dev/null || { echo "cannot start the master"; exit 1; }
$ENG run -d --name s331-rr --network container:s331-rm "$IMG" \
	redis-server --port 6380 --replicaof 127.0.0.1 6379 --save "" --appendonly no > /dev/null \
	|| { echo "cannot start the replica"; exit 1; }
RM() { $ENG exec s331-rm redis-cli -p 6379 "$@" | tr -d '\r'; }
RR() { $ENG exec s331-rr redis-cli -p 6380 "$@" | tr -d '\r'; }
linkup() { k=0; until RR INFO replication 2>/dev/null | grep -q "master_link_status:up"; do
	sleep 0.2; k=$((k+1)); [ $k -gt 100 ] && return 1; done; return 0; }
linkup || { echo "the replica never linked"; exit 1; }
echo "redis: $(RM INFO server | grep -m1 redis_version), master + replica"
# ACLs are not replicated: the read-only user on each server, the GUIDE's
# line; and one without +info for case 5
for s in RM RR; do
	$s ACL SETUSER reader on '>rpw-repl' '~*' +@read +hello +auth +select +ping +info '+client|id' '+client|tracking' >/dev/null
	$s ACL SETUSER noinfo on '>rpw-repl' '~*' +@read +hello +auth +select +ping '+client|id' '+client|tracking' >/dev/null
done
RUN_M=$(RM INFO server | sed -n 's/^run_id://p'); RUN_R=$(RR INFO server | sed -n 's/^run_id://p')
RM SET tok:a a1 >/dev/null
RM HSET tok:h f v >/dev/null
RM SET tok:d dval >/dev/null
RM SET tok:p p1 >/dev/null
RM SET tok:q q1 >/dev/null
RM SET tok:n n1 >/dev/null
k=0; until [ "$(RR GET tok:n)" = n1 ]; do sleep 0.1; k=$((k+1)); [ $k -gt 50 ] && break; done
$ENG exec s331-rm redis-cli -p 6379 MONITOR > "$D/mon-m.log" 2>&1 &
M1=$!
$ENG exec s331-rr redis-cli -p 6380 MONITOR > "$D/mon-r.log" 2>&1 &
M2=$!
sleep 0.3

cat > "$D/rc.py" <<'PY'
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
PY
# a round-robin TCP proxy: each new connection goes to the next server
cat > "$D/rr.py" <<'PY'
import socket, sys, threading
lp, backs = int(sys.argv[1]), [int(p) for p in sys.argv[2:]]
n = [0]
def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    for x in (a, b):
        try:
            x.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", lp)); s.listen(16)
while True:
    c, _ = s.accept()
    b = socket.create_connection(("127.0.0.1", backs[n[0] % len(backs)])); n[0] += 1
    threading.Thread(target=pipe, args=(c, b), daemon=True).start()
    threading.Thread(target=pipe, args=(b, c), daemon=True).start()
PY
rc() { python3 "$D/rc.py" $RESP "$@" 2>&1; }
stat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
print([c for c in r["collections"] if c["name"]=="0"][0]["upstream"][sys.argv[1]])' "$1" 2>/dev/null; }
# start_node <user> <port> <conns>: a fresh node (state and log) reading
# the upstream at 127.0.0.1:<port>
start_node() {
	[ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
	rm -rf "$D/state"; mkdir -p "$D/state/wal"
	cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = repl-client-secret
upstream = rpw-repl
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
fsync = everysec
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://$1@127.0.0.1:$2
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
upstream_negative_ms = 2000
upstream_conns = $3
C
	chmod 600 "$D/n.conf"
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	PID=$!
	k=0; until grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; k=$((k+1)); [ $k -gt 100 ] && { tail -5 "$D/n.log"; return 1; }; done
	return 0
}
# until <seconds> <want> <command...>: poll a read until it answers <want>;
# prints the answer and the milliseconds it took
until_is() { t=$1; w=$2; shift 2; k=0; while :; do a=$(rc "$@"); [ "$a" = "$w" ] && break
	[ $k -ge $((t * 10)) ] && break; sleep 0.1; k=$((k+1)); done; echo "$a $((k * 100))"; }

echo "--- 1: the replica, as the read-only user"
start_node reader $RP 2 || exit 1
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 50 ]; do sleep 0.1; k=$((k+1)); done
[ "$(stat tracking)" = True ] && [ "$(stat server)" = "$RUN_R" ] \
	&& ok "tracking on against the replica; stats name its run_id ($(echo $RUN_R | cut -c1-12))" \
	|| bad "tracking $(stat tracking), server '$(stat server)', replica $RUN_R"

echo "--- 2: the master's changes reach perfcached through the replica"
A=$(rc 'GET tok:a'); RM SET tok:a a2 >/dev/null
set -- $(until_is 3 "'a2'" 'GET tok:a')
[ "$A" = "'a1'" ] && [ "$1" = "'a2'" ] && [ "$(stat invalidations)" -ge 1 ] \
	&& ok "SET on the master: the replica's invalidation arrived, the next read has a2 ($2 ms)" \
	|| bad "SET on the master: first $A, then $1 after $2 ms (invalidations $(stat invalidations))"
A=$(rc 'HGETALL tok:h'); RM DEL tok:h >/dev/null
set -- $(until_is 3 "[]" 'HGETALL tok:h')
[ "$A" = "['f', 'v']" ] && [ "$1" = "[]" ] \
	&& ok "DEL on the master: gone here too ($2 ms)" || bad "DEL on the master: first $A, then $1"
RM SET tok:e ev PX 1500 >/dev/null
k=0; until [ "$(RR GET tok:e)" = ev ]; do sleep 0.05; k=$((k+1)); [ $k -gt 40 ] && break; done
A=$(rc 'GET tok:e')
set -- $(until_is 4 None 'GET tok:e')
[ "$A" = "'ev'" ] && [ "$1" = None ] \
	&& ok "a key expiring on the master: read, then gone ($2 ms after the read)" || bad "expiry: first $A, then $1"

acl() { echo "$(RM ACL LOG | grep -c .)$(RR ACL LOG | grep -c .)"; }
AL1=$(acl)
echo "--- 3: the replica restarts"
F0=$(stat flushes)
A=$(rc 'GET tok:q')
$ENG restart s331-rr >/dev/null 2>&1
linkup || bad "the replica did not link again"
RUN_R2=$(RR INFO server | sed -n 's/^run_id://p')
for s in RR; do
	$s ACL SETUSER reader on '>rpw-repl' '~*' +@read +hello +auth +select +ping +info '+client|id' '+client|tracking' >/dev/null
	$s ACL SETUSER noinfo on '>rpw-repl' '~*' +@read +hello +auth +select +ping '+client|id' '+client|tracking' >/dev/null
done
RR ACL LOG RESET >/dev/null
kill $M2 2>/dev/null
$ENG exec s331-rr redis-cli -p 6380 MONITOR >> "$D/mon-r.log" 2>&1 &
M2=$!
RM SET tok:q q2 >/dev/null
k=0; while [ "$(stat server)" != "$RUN_R2" ] && [ $k -lt 100 ]; do sleep 0.1; k=$((k+1)); done
set -- $(until_is 3 "'q2'" 'GET tok:q')
[ "$A" = "'q1'" ] && [ "$(stat flushes)" -gt "${F0:-0}" ] && [ "$(stat tracking)" = True ] \
	&& [ "$(stat server)" = "$RUN_R2" ] && [ "$RUN_R2" != "$RUN_R" ] && [ "$1" = "'q2'" ] \
	&& ok "copies dropped, tracking back on the restarted replica (new run_id $(echo $RUN_R2 | cut -c1-12)), the change read" \
	|| bad "replica restart: first $A, flushes $F0 -> $(stat flushes), tracking $(stat tracking), server $(stat server | cut -c1-12) (want $(echo $RUN_R2 | cut -c1-12)), then $1"

echo "--- 4: a round-robin proxy over master and replica"
python3 "$D/rr.py" $XP $MP $RP > "$D/rr.log" 2>&1 &
XPID=$!
sleep 0.3
start_node reader $XP 1 || exit 1
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 50 ]; do sleep 0.1; k=$((k+1)); done
A=$(rc 'GET tok:p'); RM SET tok:p p2 >/dev/null
set -- $(until_is 4 "'p2'" 'GET tok:p')
[ "$A" = "'p1'" ] && [ "$1" = "'p2'" ] && [ "$(stat tracking_other_server)" -ge 1 ] 2>/dev/null \
	&& grep -q "reached another server" "$D/n.log" \
	&& ok "the fetching connection reached the other server: not tracked, the change read within upstream_negative_ms ($2 ms; tracking_other_server $(stat tracking_other_server))" \
	|| bad "round-robin: first $A, then $1 after $2 ms, tracking_other_server $(stat tracking_other_server), log $(grep -c 'reached another server' "$D/n.log")"
kill $XPID 2>/dev/null; XPID=

AL2=$(acl)
[ "$AL1" = 00 ] && [ "$AL2" = 00 ] && ok "ACL LOG empty on both servers (before the replica restart, and since)" \
	|| bad "ACL LOG lines (master, replica): $AL1 before the restart, $AL2 since"
echo "--- 5: a user without +info"
start_node noinfo $RP 2 || exit 1
k=0; while ! grep -q "does not say which server" "$D/n.log" && [ $k -lt 50 ]; do sleep 0.1; k=$((k+1)); done
A=$(rc 'GET tok:a')
[ "$(stat tracking)" = False ] && [ "$A" = "'a2'" ] && grep -q "does not say which server" "$D/n.log" \
	&& ok "no +info: tracking off, and the log says why; reads work ($A)" \
	|| bad "no +info: tracking $(stat tracking), read $A"
L=$(RR ACL LOG | tr '\n' ' ')
case "$L" in *noinfo*info*|*info*noinfo*) ok "and the replica's ACL LOG shows the INFO it refused to noinfo" ;;
*) bad "replica ACL LOG: $(echo "$L" | cut -c1-200)" ;; esac

echo "--- 6: one way"
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
sleep 0.5
kill $M1 $M2 2>/dev/null; M1= M2=
# perfcached's connections: every command the monitors saw from anywhere
# but redis-cli inside the containers (127.0.0.1) and the replication link
python3 - "$D/mon-m.log" "$D/mon-r.log" <<'P' > "$D/mon.out"
import re, sys
reads = {"HELLO","AUTH","SELECT","PING","INFO","TYPE","PTTL","GET","HGETALL","JSON.GET","CLIENT"}
seen, bad = {}, []
for fn in sys.argv[1:]:
    for l in open(fn, errors="replace"):
        m = re.match(r'^\S+ \[\d+ ([^\]]+)\] "([^"]+)"', l)
        if not m or m.group(1).startswith("127.0.0.1:") or m.group(1) == "lua":
            continue
        c = m.group(2).upper()
        seen[c] = seen.get(c, 0) + 1
        if c not in reads:
            bad.append(l.strip()[:120])
print(" ".join("%s=%d" % kv for kv in sorted(seen.items())))
print("BAD " + " | ".join(bad[:5]) if bad else "NOBAD")
P
SEEN=$(sed -n 1p "$D/mon.out"); B2=$(sed -n 2p "$D/mon.out")
[ -n "$SEEN" ] && [ "$B2" = NOBAD ] && ok "MONITOR on both: only the handshake and reads ($SEEN)" || bad "MONITOR: $SEEN / $B2"
echo "replica-interop: $pass passed, $fail failed"
[ $fail -eq 0 ]
