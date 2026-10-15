#!/bin/sh
# upfailovertest.sh - S382 part 2 (0.6.1): upstream_mode = failover.  Two
# replica fakes (test/fakeupstream.py), listed R2 then R1, with
# upstream_primary = R1 (the second listed - the key decides, not the
# order) and upstream_preempt_hold_s = 15; PERFCACHED_ALERT_FAST=60.
#  1. /stats: mode failover, the primary R1, R1 active; every read goes to
#     R1, none to the standby R2
#  2. R1 down: R2 takes over (logged "failed over"), serves every read; the
#     failover alert names it
#  3. R1 back, upstream_preempt = yes: R2 stays active through the hold,
#     then R1 takes over again (logged); the alert clears; reads go to R1
#  4. upstream_preempt = no: after a failover R2 stays active although R1
#     serves again; the alert clears (the primary serves)
#  5. config: a primary that is not listed, the failover keys without
#     failover, a hold under 15 s, a bad mode - each refused
# Fail-first: 0.6.0.1 refuses upstream_mode as an unknown key.
# Usage: test/upfailovertest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupfo.XXXXXX)
P1=19891 P2=19892 TCP=19893 RESP=19894 HTTP=19895
F1= F2= PID=
trap 'for p in $PID $F1 $F2; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $P1 $P2 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upfailovertest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60

python3 - "$D" <<'PY'
import json, os, sys
d = {"k:%d" % i: {"type": "string", "value": "v%d" % i} for i in range(1, 201)}
for n in (1, 2):
    os.makedirs("%s/up%d" % (sys.argv[1], n))
    json.dump(d, open("%s/up%d/data.json" % (sys.argv[1], n), "w"))
PY
ctl() { echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"; }
ctl 1 '{"role": "replica"}'
ctl 2 '{"role": "replica"}'
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done

conf() { # conf <name> [collection lines...]
	n=$1; shift
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = upfo-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P2, redis://127.0.0.1:$P1
upstream_role = replica
upstream_prefixes = k:*
upstream_timeout_ms = 1000
upstream_negative_ms = 1000
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
start() {
	rm -rf "$D/state"; mkdir -p "$D/state"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
up() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
alert() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
print(next((a["level"] for a in s["alerts"]["active"] if a["key"]==sys.argv[1]), "none"))' "$1" 2>/dev/null; }
gets() { g_=$(grep -c '^GET ' "$D/up$1/counts" 2>/dev/null); echo "${g_:-0}"; }
reads() { # reads <first> <last> - "<right> <wrong>"
	python3 - $RESP "$1" "$2" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
f = s.makefile("rb")
good = bad = 0
for i in range(int(sys.argv[2]), int(sys.argv[3]) + 1):
    k = b"k:%d" % i
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(k), k))
    l = f.readline()
    if l.startswith(b"$") and not l.startswith(b"$-1"):
        good += f.read(int(l[1:]) + 2)[:-2] == b"v%d" % i
    else:
        bad += 1
print(good, bad)
PY
}
waitfor() { w=0; while [ $w -lt $(($1 * 10)) ]; do eval "$2" && return 0; sleep 0.1; w=$((w+1)); done; return 1; }
ACT='u.get("active")'
R1=127.0.0.1:$P1 R2=127.0.0.1:$P2

# ---- 1. the primary serves ------------------------------------------------------------
conf a "upstream_mode = failover" "upstream_primary = $R1" "upstream_preempt_hold_s = 15"
if ! start a; then
	bad "did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/a.log")"; echo "upfailovertest: $pass passed, $fail failed"; exit 1
fi
waitfor 5 '[ "$(up "$ACT")" = "$R1" ]'
[ "$(up 'u["mode"]')" = failover ] && [ "$(up 'u["primary"]')" = "$R1" ] && [ "$(up "$ACT")" = "$R1" ] &&
	ok "1. /stats: failover, primary $R1 (listed second), active" || bad "1. /stats: mode $(up 'u.get("mode")') primary $(up 'u.get("primary")') active $(up "$ACT")"
r=$(reads 1 30)
[ "$r" = "30 0" ] && [ "$(gets 1)" -ge 30 ] && [ "$(gets 2)" = 0 ] && ok "1. every read to the primary ($(gets 1)), none to the standby" ||
	bad "1. reads $r, R1 $(gets 1), R2 $(gets 2)"

# ---- 2. the primary down: the standby takes over --------------------------------------
ctl 1 '{"role": "replica", "down": true, "kill_seq": 1}'
waitfor 8 '[ "$(up "$ACT")" = "$R2" ]'
[ "$(up "$ACT")" = "$R2" ] && grep -q "failed over from $R1 to $R2" "$D/a.log" && ok "2. R1 down: R2 active, logged" ||
	bad "2. active $(up "$ACT"); log: $(grep -m1 'failed over' "$D/a.log")"
g2=$(gets 2)
r=$(reads 31 50)
[ "$r" = "20 0" ] && [ $(($(gets 2) - g2)) -ge 20 ] && ok "2. R2 serves every read" || bad "2. reads $r, R2 +$(($(gets 2) - g2))"
waitfor 5 '[ "$(alert upstream.0.failover)" = warning ]'
[ "$(alert upstream.0.failover)" = warning ] && ok "2. the failover alert: upstream.0.failover" || bad "2. alert: $(alert upstream.0.failover)"

# ---- 3. the primary back: the hold, then it takes over -------------------------------------
ctl 1 '{"role": "replica"}'
sleep 6
[ "$(up "$ACT")" = "$R2" ] && ok "3. R1 back: R2 stays active through the hold (6 s of 15)" || bad "3. active $(up "$ACT") before the hold ran out"
waitfor 20 '[ "$(up "$ACT")" = "$R1" ]'
[ "$(up "$ACT")" = "$R1" ] && grep -q "primary $R1 is back - it takes over from $R2" "$D/a.log" &&
	ok "3. after the hold R1 takes over again, logged" || bad "3. active $(up "$ACT"); log: $(grep -m1 'is back' "$D/a.log")"
waitfor 5 '[ "$(alert upstream.0.failover)" = none ]'
[ "$(alert upstream.0.failover)" = none ] && ok "3. the failover alert cleared" || bad "3. alert: $(alert upstream.0.failover)"
g1=$(gets 1)
r=$(reads 51 60)
[ "$r" = "10 0" ] && [ $(($(gets 1) - g1)) -ge 10 ] && ok "3. reads go to R1 again" || bad "3. reads $r, R1 +$(($(gets 1) - g1))"
[ "$(up 'u["failovers"]')" = 1 ] && ok "3. /stats failovers 1" || bad "3. failovers $(up 'u.get("failovers")')"
stop

# ---- 4. upstream_preempt = no -----------------------------------------------------------------
conf b "upstream_mode = failover" "upstream_primary = $R1" "upstream_preempt = no"
if start b; then
	waitfor 5 '[ "$(up "$ACT")" = "$R1" ]'
	# no read first: the switch must not need traffic (the 10 s check)
	ctl 1 '{"role": "replica", "down": true, "kill_seq": 2}'
	waitfor 15 '[ "$(up "$ACT")" = "$R2" ]'
	[ "$(up "$ACT")" = "$R2" ] && ok "4. no traffic: the dead primary is seen within 15 s, R2 active" ||
		bad "4. idle: active still $(up "$ACT") after 15 s"
	ctl 1 '{"role": "replica"}'
	waitfor 8 '[ "$(up "[x[\"state\"] for x in u[\"upstreams\"]][1]")" = serving ]'
	sleep 4
	[ "$(up "$ACT")" = "$R2" ] && [ "$(up '[x["state"] for x in u["upstreams"]][1]')" = serving ] &&
		ok "4. preempt off: R1 serves again, R2 stays active" || bad "4. active $(up "$ACT"), R1 $(up '[x["state"] for x in u["upstreams"]][1]')"
	waitfor 5 '[ "$(alert upstream.0.failover)" = none ]'
	[ "$(alert upstream.0.failover)" = none ] && ok "4. the failover alert cleared: the primary serves" || bad "4. alert $(alert upstream.0.failover)"
	stop
else
	bad "4. did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/b.log")"
fi

# ---- 5. config ------------------------------------------------------------------------------------
cfgerr() { # cfgerr <name> <message> <lines...>
	n=$1 msg=$2; shift 2
	conf "$n" "$@"
	"$BIN" -C -f "$D/$n.conf" > "$D/$n.out" 2>&1
	[ $? -ne 0 ] && grep -qF "$msg" "$D/$n.out"
}
cfgerr c1 "upstream_primary = 127.0.0.1:1 is not one of its upstreams" "upstream_mode = failover" "upstream_primary = 127.0.0.1:1" &&
	ok "5. a primary not listed is refused" || bad "5. primary: $(tail -1 "$D/c1.out")"
cfgerr c2 "need upstream_mode = failover" "upstream_preempt = no" && ok "5. the failover keys without failover are refused" ||
	bad "5. balance: $(tail -1 "$D/c2.out")"
cfgerr c3 "'10' is not an integer in [15..300]" "upstream_mode = failover" "upstream_preempt_hold_s = 10" && ok "5. a hold under 15 s is refused" ||
	bad "5. hold: $(tail -1 "$D/c3.out")"
cfgerr c4 "upstream_mode: balance|failover" "upstream_mode = active" && ok "5. a bad mode is refused" || bad "5. mode: $(tail -1 "$D/c4.out")"

w=$(cat "$D"/up*/writes 2>/dev/null | wc -l)
[ "$w" = 0 ] && ok "nothing was written to either fake" || bad "$w writes upstream"
echo "upfailovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
