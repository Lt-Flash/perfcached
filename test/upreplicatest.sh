#!/bin/sh
# upreplicatest.sh - S382 (0.6.1): several upstreams for one read-through
# collection, replicas only.  Three fakes (test/fakeupstream.py, which
# answers ROLE and INFO replication as its ctl.json says): R1 and R2
# replicas, M a master, all three in `upstream`, `upstream_role = replica`;
# PERFCACHED_ALERT_FAST=60 (the alerts' hold and clear times / 60).
#  1. the master is refused at its first connect, the replicas serve
#  2. reads spread over both replicas (each fetches some); none goes to M
#  3. R1 lost (down, its tracking connection closed): ITS copies are
#     dropped and R2's stay; R2 serves every read; R1's warning is raised,
#     the master's none (skipped by design, listed to follow a failover)
#  4. R1 back: served again, within the 2 s it is dialled at
#  5. R2 promoted (ROLE says master): refused within the 10 s ROLE check;
#     a replica again: taken back
#  6. both replicas down: the master is NOT read - the read answers as
#     upstream_on_error says, and "no upstream serves" is critical
#  7. upstream_master_fallback = yes, both replicas down: the master serves,
#     with its warning, until a replica is back
#  8. config: the fallback without upstream_role = replica, a ninth
#     upstream, upstream_role without an upstream - each refused
#  and nothing is ever written to any of them.
# Fail-first: 0.6.0.1 refuses upstream_role as an unknown key.
# Usage: test/upreplicatest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcuprep.XXXXXX)
P1=19841 P2=19842 P3=19843 TCP=19844 RESP=19845 HTTP=19846
F1= F2= F3= PID=
trap 'for p in $PID $F1 $F2 $F3; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $P1 $P2 $P3 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upreplicatest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60

# ---- three fakes, the same keyspace ---------------------------------------------
python3 - "$D" <<'PY'
import json, os, sys
d = {"k:%d" % i: {"type": "string", "value": "v%d" % i} for i in range(1, 121)}
for n in (1, 2, 3):
    os.makedirs("%s/up%d" % (sys.argv[1], n))
    json.dump(d, open("%s/up%d/data.json" % (sys.argv[1], n), "w"))
PY
ctl() { # ctl <n> <json> - fake n's behaviour (atomically)
	echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"
}
ctl 1 '{"role": "replica"}'
ctl 2 '{"role": "replica"}'
ctl 3 '{"role": "master"}'
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
python3 "$T/fakeupstream.py" $P3 "$D/up3" > "$D/f3.log" 2>&1 & F3=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ] && [ -f "$D/up3/ready" ]; }; do sleep 0.1; i=$((i+1)); done

conf() { # conf <name> [collection lines...]
	n=$1; shift
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = uprep-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$P3
upstream_role = replica
upstream_prefixes = k:*
upstream_timeout_ms = 1000
upstream_negative_ms = 2000
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
start() { # start <conf>
	rm -rf "$D/state"; mkdir -p "$D/state"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
up() { # up <python expr over u (collection 0's upstream block) and s (/stats)>
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
state() { up 'u["upstreams"]['"$(($1 - 1))"']["state"]'; }
gets() { # GETs fake n was sent (grep -c says 0 AND fails on none: one figure)
	g_=$(grep -c '^GET ' "$D/up$1/counts" 2>/dev/null); echo "${g_:-0}"; }
read_keys() { # read_keys <first> <last> - GETs at the RESP door; prints "<ok> <errors>"
	python3 - $RESP "$1" "$2" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rb")
good = errs = 0
for i in range(int(sys.argv[2]), int(sys.argv[3]) + 1):
    k = b"k:%d" % i
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(k), k))
    l = f.readline()
    if l.startswith(b"$") and not l.startswith(b"$-1"):
        v = f.read(int(l[1:]) + 2)[:-2]
        good += v == b"v%d" % i
    else:
        errs += 1                       # an error (on_error = error) or nil
print(good, errs)
PY
}
alert() { # alert <key> - its level, or none
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
print(next((a["level"] for a in s["alerts"]["active"] if a["key"]==sys.argv[1]), "none"))' "$1" 2>/dev/null
}
waitfor() { # waitfor <seconds> <shell condition>
	w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1
}

# ---- 1. the master refused, the replicas serve -------------------------------------
conf a
if ! start a; then
	bad "the node did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a.log")"
	echo "upreplicatest: $pass passed, $fail failed"; exit 1
fi
waitfor 5 '[ "$(state 3)" = "not used" ] && [ "$(state 1)" = serving ] && [ "$(state 2)" = serving ]'
[ "$(state 1)" = serving ] && [ "$(state 2)" = serving ] && [ "$(state 3)" = "not used" ] &&
	[ "$(up 'u["upstreams"][2]["why"]')" = "a master" ] &&
	ok "1. R1, R2 serving; M not used: a master" || bad "1. states: $(up '[(x["addr"], x["state"], x["why"]) for x in u["upstreams"]]')"
grep -q "upstream 127.0.0.1:$P3 is a master - not used (upstream_role = replica)" "$D/a.log" &&
	ok "1. the log says M is a master, not used" || bad "1. log: $(grep -m2 'upstream 127' "$D/a.log")"

# ---- 2. reads spread over the replicas -----------------------------------------------
r=$(read_keys 1 40)
g1=$(gets 1) g2=$(gets 2) g3=$(gets 3)
[ "$r" = "40 0" ] && ok "2. 40 reads through, every value right" || bad "2. reads: $r (good errors)"
[ "$g1" -ge 8 ] && [ "$g2" -ge 8 ] && [ "$g3" = 0 ] && [ $((g1 + g2)) -ge 40 ] &&
	ok "2. spread: R1 fetched $g1, R2 $g2, the master 0" || bad "2. fetches R1 $g1, R2 $g2, M $g3"

# ---- 3. R1 lost: its copies go, R2's stay ------------------------------------------------
e1=$(up 'u["upstreams"][0]["shadow_entries"]') e2=$(up 'u["upstreams"][1]["shadow_entries"]')
ctl 1 '{"role": "replica", "down": true, "kill_seq": 1}'
waitfor 5 '[ "$(up "u[\"upstreams\"][0][\"shadow_entries\"]")" = 0 ]'
[ "$e1" -gt 0 ] && [ "$(up 'u["upstreams"][0]["shadow_entries"]')" = 0 ] &&
	[ "$(up 'u["upstreams"][1]["shadow_entries"]')" -ge "$e2" ] &&
	ok "3. R1 lost: its $e1 copies dropped, R2's $e2 kept" ||
	bad "3. copies R1 $e1 -> $(up 'u["upstreams"][0]["shadow_entries"]'), R2 $e2 -> $(up 'u["upstreams"][1]["shadow_entries"]')"
g2=$(gets 2)
r=$(read_keys 41 60)
[ "$r" = "20 0" ] && [ $(($(gets 2) - g2)) -ge 20 ] && [ "$(gets 3)" = 0 ] &&
	ok "3. R2 serves every read ($(($(gets 2) - g2)) fetches), the master none" || bad "3. reads $r, R2 +$(($(gets 2) - g2)), M $(gets 3)"
waitfor 6 '[ "$(alert upstream.0.1)" = warning ]'
[ "$(alert upstream.0.1)" = warning ] && ok "3. R1's warning: upstream.0.1" || bad "3. upstream.0.1: $(alert upstream.0.1)"
# the master, skipped by design since 1. (listed to follow a failover), raises none
[ "$(alert upstream.0.3)" = none ] && ok "3. the master, skipped by design, raises no alert: upstream.0.3 none" ||
	bad "3. upstream.0.3: $(alert upstream.0.3)"

# ---- 4. R1 back ---------------------------------------------------------------------------
ctl 1 '{"role": "replica"}'
waitfor 8 '[ "$(state 1)" = serving ]'
[ "$(state 1)" = serving ] && ok "4. R1 back: serving" || bad "4. R1: $(state 1)"

# ---- 5. R2 promoted, then a replica again -----------------------------------------------
ctl 2 '{"role": "master"}'
waitfor 13 '[ "$(state 2)" = "not used" ]'
[ "$(state 2)" = "not used" ] && grep -q "upstream 127.0.0.1:$P2 is a master - not used" "$D/a.log" &&
	ok "5. R2 says master: refused within the 10 s check" || bad "5. R2: $(state 2)"
ctl 2 '{"role": "replica"}'
waitfor 13 '[ "$(state 2)" = serving ]'
[ "$(state 2)" = serving ] && grep -q "upstream 127.0.0.1:$P2 is a replica in step again - taken back" "$D/a.log" &&
	ok "5. a replica again: taken back" || bad "5. R2: $(state 2); $(grep 'taken back' "$D/a.log" | tail -1)"

# ---- 6. both replicas down: the master is not read ---------------------------------------
ctl 1 '{"role": "replica", "down": true, "kill_seq": 2}'
ctl 2 '{"role": "replica", "down": true, "kill_seq": 1}'
waitfor 6 '[ "$(state 1)" = "not used" ] && [ "$(state 2)" = "not used" ]'
g3=$(gets 3)
r=$(read_keys 61 62)
[ "$r" = "0 2" ] && [ "$(gets 3)" = "$g3" ] && ok "6. both replicas down: the reads answer an error, the master is not read" ||
	bad "6. reads $r, M fetched $(($(gets 3) - g3))"
waitfor 6 '[ "$(alert upstream.0)" = critical ]'
[ "$(alert upstream.0)" = critical ] && ok "6. 'no upstream serves' is critical" || bad "6. upstream.0: $(alert upstream.0)"
stop

# ---- 7. the master fallback ------------------------------------------------------------------
conf b "upstream_master_fallback = yes"
if start b; then
	# the replicas are "not reached yet" until their first dial fails -
	# tried before the master, so the reads wait for that
	waitfor 6 '[ "$(state 3)" = "master fallback" ] && [ "$(state 1)" = "not used" ] && [ "$(state 2)" = "not used" ]'
	r=$(read_keys 63 70)
	[ "$r" = "8 0" ] && [ "$(gets 3)" -ge 8 ] && [ "$(state 3)" = "master fallback" ] &&
		ok "7. no replica: the master serves ($(gets 3) fetches), state 'master fallback'" ||
		bad "7. reads $r, M $(gets 3), state $(state 3)"
	waitfor 6 '[ "$(alert upstream.0.master)" = warning ]'
	[ "$(alert upstream.0.master)" = warning ] && ok "7. its warning: upstream.0.master" || bad "7. upstream.0.master: $(alert upstream.0.master)"
	ctl 1 '{"role": "replica"}'
	waitfor 8 '[ "$(state 1)" = serving ]'
	g3=$(gets 3)
	r=$(read_keys 71 80)
	[ "$r" = "10 0" ] && [ "$(gets 3)" = "$g3" ] && ok "7. R1 back: it serves, the master no more" ||
		bad "7. reads $r, M +$(($(gets 3) - g3)), R1 $(state 1)"
	waitfor 6 '[ "$(alert upstream.0.master)" = none ]'
	[ "$(alert upstream.0.master)" = none ] && ok "7. the master's warning cleared" || bad "7. still $(alert upstream.0.master)"
	stop
else
	bad "7. did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/b.log")"
fi

# ---- 8. config ---------------------------------------------------------------------------------
conf c "upstream_role = any" "upstream_master_fallback = yes"
"$BIN" -C -f "$D/c.conf" > "$D/c.out" 2>&1
[ $? -ne 0 ] && grep -q "upstream_master_fallback = yes needs upstream_role = replica" "$D/c.out" &&
	ok "8. the fallback without upstream_role = replica is refused" || bad "8. fallback/any: $(tail -1 "$D/c.out")"
L=""; for i in 1 2 3 4 5 6 7 8 9; do L="$L${L:+, }redis://127.0.0.1:$((20000 + i))"; done
sed "s|^upstream = .*|upstream = $L|" "$D/a.conf" > "$D/d.conf"; chmod 600 "$D/d.conf"
"$BIN" -C -f "$D/d.conf" > "$D/d.out" 2>&1
[ $? -ne 0 ] && grep -q "upstream: at most 8" "$D/d.out" && ok "8. a ninth upstream is refused" || bad "8. nine: $(tail -1 "$D/d.out")"
printf '[daemon]\nworkers = 1\n[secrets]\nclient = x-client-secret\n[listen]\ntcp = 127.0.0.1:%s\nplaintext = loopback\n[collection 0]\nbuckets_log2 = 8\nupstream_role = replica\n' $TCP > "$D/e.conf"
chmod 600 "$D/e.conf"
"$BIN" -C -f "$D/e.conf" > "$D/e.out" 2>&1
[ $? -ne 0 ] && grep -q "upstream_role / upstream_max_lag_ms without an upstream" "$D/e.out" &&
	ok "8. upstream_role without an upstream is refused" || bad "8. role alone: $(tail -1 "$D/e.out")"

# ---- never a write ----------------------------------------------------------------------------
w=$(cat "$D"/up*/writes 2>/dev/null | wc -l)
[ "$w" = 0 ] && ok "nothing was written to any upstream" || bad "$w writes: $(cat "$D"/up*/writes | head -3)"
echo "upreplicatest: $pass passed, $fail failed"
[ $fail -eq 0 ]
