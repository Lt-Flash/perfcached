#!/bin/sh
# dashtest.sh - DESIGN 12no (0.6.6, S407/S409): perfcached-dashboard serves
# the status page from its own process, over libperfd, for a two-member
# fleet with a write-through collection on fakes (test/fakeupstream.py).
#
#  1. the page: GET / is the daemon's page byte for byte, under the SAME
#     Content-Security-Policy the daemon computes (the script's SHA-256)
#  2. the trees: /stats, /members, /clients are the selected member's;
#     ?node=N selects; /session names the tool and both members
#  3. the gates: a POST without the custom header is refused (403), a
#     non-JSON content type (415), a body over body_max (413), an unknown
#     route (404), another method (405); a wrong enable secret is 401 and
#     lockout_after of them lock the address out (429); the right secret
#     opens a session; an action without the token is 401
#  4. the actions: check, upfill and stop reach the daemon's upfill verb
#     (its answer comes back; the daemon logged the raised privilege);
#     reset-stats too; a bad op is 400
#  5. a member that goes away is reported, the other keeps answering;
#     TERM stops the tool cleanly
# The browser side (the buttons, the session prompt) is the page's script,
# which pagejstest.sh executes; this suite drives the tool with curl.
# Usage: test/dashtest.sh ./perfcached ./perfcached-dashboard
set -u
BIN=${1:-./perfcached}
DASH=${2:-./perfcached-dashboard}
T=$(cd "$(dirname "$0")" && pwd)
[ -x "$DASH" ] || { echo "dashtest: SKIPPED - $DASH not built (libmicrohttpd-dev absent?)"; echo "dashtest: the dashboard was NOT tested here"; exit 0; }
command -v curl >/dev/null 2>&1 || { echo "dashtest: curl is needed"; exit 1; }
D=$(mktemp -d /var/tmp/pcdash.XXXXXX)
PM=17611 P1=17612 P2=17613 TCP1=17601 TCP2=17602 HTTP1=17681 HP=17690
FM= F1= F2= PA= PB= PD= PD2= PD3=
trap 'for p in $PA $PB $PD $PD2 $PD3 $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
HP2=17691 HP3=17692 DEAD=17609   # S415: two more instances; a seed nobody serves
for p in $PM $P1 $P2 $TCP1 $TCP2 $HTTP1 $HP $HP2 $HP3 $DEAD; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "dashtest: port $p already bound" >&2; exit 1; }
done

# ---- the fakes: M the master, R1 and R2 its replicas ---------------------
ctl() { echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"; }
for n in m 1 2; do mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"; done
ctl m '{"role": "master", "accept_writes": true, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done

# ---- two members on loopback aliases, a through collection on both -------
node() { # node <n> <tcp> [http]
	mkdir -p "$D/state$1"
	{
		cat <<C
[daemon]
workers = 2
log_level = info
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = dash-client-secret
cluster = dash-cluster-secret
enable = dash-enable-secret
[listen]
tcp = 127.0.0.6$1:$2
C
		[ -n "${3:-}" ] && echo "http = 127.0.0.1:$3"
		cat <<C
[cluster]
multicast = 239.255.77.61:17661
advertise = 127.0.0.6$1
pull_timeout_ms = 200
negative_ms = 500
tombstone_ms = 2000
[collection wt]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 1000
C
	} > "$D/n$1.conf"
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
node 1 $TCP1 $HTTP1
node 2 $TCP2
start 1 A
start 2 B
sleep 3.5   # the join window, the election, the first keepalives

# ---- the tool ---------------------------------------------------------------
# S415: the first seed is dead (nobody serves $DEAD) - the second gets the
# fleet; the session key is shared with a second instance later
KEY=$(python3 -c 'import os; print(os.urandom(32).hex())')
cat > "$D/dash.conf" <<C
[dashboard]
listen = 127.0.0.1:$HP
seed = 127.0.0.69:$DEAD, 127.0.0.61:$TCP1
secret = dash-client-secret
session_key = $KEY
cache_ms = 300
refresh_ms = 500
body_max = 1024
lockout_after = 3
lockout_s = 2
trusted_proxy = 127.0.0.1
C
chmod 600 "$D/dash.conf"
"$DASH" -f "$D/dash.conf" > "$D/dash.out" 2> "$D/dash.log" & PD=$!
i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached-dashboard ready" "$D/dash.out" && break
	kill -0 $PD 2>/dev/null || { echo "the tool died:"; cat "$D/dash.log"; exit 1; }
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached-dashboard ready" "$D/dash.out" && ok "the tool is up on $HP" || { bad "the tool did not say ready"; cat "$D/dash.log"; }

U="http://127.0.0.1:$HP"
# req <method> <path> [curl args...] -> status in $ST, body in $D/body, headers in $D/hdr
req() {
	m=$1; p=$2; shift 2
	ST=$(curl -s -o "$D/body" -D "$D/hdr" -w '%{http_code}' -X "$m" "$@" "$U$p")
}
hdr() { tr -d '\r' < "$D/hdr" | grep -i "^$1:" | head -1 | cut -d: -f2- | sed 's/^ *//'; }
jq1() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$D/body" 2>/dev/null; }

echo "--- 1. the page"
req GET /
[ "$ST" = 200 ] && ok "GET / 200" || bad "GET / $ST"
hdr Content-Type | grep -q "text/html" && ok "text/html" || bad "content type $(hdr Content-Type)"
curl -s -o "$D/daemonpage" -D "$D/dhdr" "http://127.0.0.1:$HTTP1/"
cmp -s "$D/body" "$D/daemonpage" && ok "the page is the daemon's, byte for byte" || bad "the page differs from the daemon's"
DCSP=$(tr -d '\r' < "$D/dhdr" | grep -i "^Content-Security-Policy:" | cut -d: -f2- | sed 's/^ *//')
TCSP=$(hdr Content-Security-Policy)
[ -n "$TCSP" ] && [ "$TCSP" = "$DCSP" ] && ok "the CSP is the daemon's: $(echo "$TCSP" | cut -c1-60)..." || bad "CSP differs: tool [$TCSP] daemon [$DCSP]"
echo "$TCSP" | grep -q "script-src 'sha256-" && ok "script-src is a hash" || bad "no script hash in the CSP"
hdr X-Content-Type-Options | grep -q nosniff && ok "nosniff" || bad "no nosniff"
hdr X-Frame-Options | grep -q DENY && ok "no framing" || bad "framing allowed"

echo "--- 2. the trees"
req GET /stats
[ "$ST" = 200 ] && ok "/stats 200" || { bad "/stats $ST: $(cat "$D/body")"; }
[ "$(jq1 "$D/body" "'collections' in d")" = True ] && ok "/stats has collections" || bad "/stats lacks collections: $(head -c 200 "$D/body")"
hdr Content-Type | grep -q "application/json" && ok "/stats is application/json" || bad "/stats content type"
# the member list needs both members seen by the seed, and the tool to have learned it
i=0; N=0
while [ $i -lt 60 ]; do
	req GET /session
	N=$(jq1 "$D/body" "len([n for n in d.get('nodes',[]) if n.get('state')=='connected'])")
	[ "$N" = 2 ] && break
	sleep 0.25; i=$((i+1))
done
[ "$ST" = 200 ] && ok "/session 200" || bad "/session $ST"
[ "$(jq1 "$D/body" "d['tool']")" = perfcached-dashboard ] && ok "/session names the tool" || bad "/session: $(cat "$D/body")"
[ "$N" = 2 ] && ok "/session lists both members connected (and the dead seed)" || bad "/session lists $N connected members: $(cat "$D/body")"
[ "$(jq1 "$D/body" "d['enabled']")" = False ] && ok "no privileged session yet" || bad "enabled before any secret"
N1=$(jq1 "$D/body" "d['node']")
req GET /members
[ "$(jq1 "$D/body" "len(d['members'])")" = 2 ] && ok "/members has two entries" || bad "/members: $(head -c 300 "$D/body")"
SELF1=$(jq1 "$D/body" "[m['node'] for m in d['members'] if m.get('self')][0]")
[ "$SELF1" = "$N1" ] && ok "the default member is node $N1 (the seed)" || bad "default member $SELF1, /session said $N1"
OTHER=$(jq1 "$D/body" "[m['node'] for m in d['members'] if not m.get('self')][0]")
req GET "/members?node=$OTHER"
[ "$ST" = 200 ] && [ "$(jq1 "$D/body" "[m['node'] for m in d['members'] if m.get('self')][0]")" = "$OTHER" ] && ok "?node=$OTHER reads the other member" || bad "?node=$OTHER: $ST $(head -c 200 "$D/body")"
req GET "/stats?node=$OTHER"
[ "$ST" = 200 ] && ok "/stats?node=$OTHER 200" || bad "/stats?node=$OTHER $ST"
req GET /clients
[ "$ST" = 200 ] && [ "$(head -c 1 "$D/body")" = "{" ] && ok "/clients 200, JSON" || bad "/clients $ST"
req GET /health
[ "$ST" = 200 ] && ok "/health 200" || bad "/health $ST"

echo "--- 3. the gates"
req GET /nope
[ "$ST" = 404 ] && ok "unknown route 404" || bad "unknown route $ST"
req PUT /stats
[ "$ST" = 405 ] && ok "PUT 405" || bad "PUT $ST"
req POST /session -H 'Content-Type: application/json' --data '{"secret":"x"}'
[ "$ST" = 403 ] && ok "POST without the header 403" || bad "POST without the header $ST"
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: text/plain' --data 'secret=x'
[ "$ST" = 415 ] && ok "text/plain 415" || bad "text/plain $ST"
BIG=$(python3 -c "print('{\"secret\":\"' + 'a'*1500 + '\"}')")
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data "$BIG"
[ "$ST" = 413 ] && ok "a body over body_max 413" || bad "big body $ST"
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{"nosecret":1}'
[ "$ST" = 400 ] && ok "no secret in the body 400" || bad "no secret $ST"
for k in 1 2 3; do
	req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{"secret":"wrong-secret"}'
	[ "$ST" = 401 ] || break
done
[ "$ST" = 401 ] && ok "a wrong secret is 401, three times" || bad "wrong secret: $ST $(cat "$D/body")"
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{"secret":"dash-enable-secret"}'
[ "$ST" = 429 ] && ok "the fourth try is locked out (429), even with the right secret" || bad "after 3 wrong: $ST"
grep -q "locked out" "$D/dash.log" && ok "the lockout is logged" || bad "no lockout log line"
# S416: from a trusted proxy the client is the last X-Forwarded-For address - its own counter, so
# 127.0.0.1's lockout does not cover it, and a wrong secret from it is counted under its name
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' -H 'X-Forwarded-For: 198.51.100.7, 203.0.113.9' --data '{"secret":"wrong-secret"}'
[ "$ST" = 401 ] && ok "a forwarded client behind the trusted proxy has its own lockout counter (401, not 429)" || bad "forwarded client: $ST"
grep -q "203.0.113.9: enable refused" "$D/dash.log" && ok "the wrong secret is logged under the forwarded address" || bad "no log line for 203.0.113.9: $(grep 'enable refused' "$D/dash.log" | tail -1)"
grep -c "wrong enable secret" "$D/n1.log" | grep -qE '^[1-9]' && ok "the daemon saw the wrong secrets" || bad "the daemon logged no wrong secret"
sleep 2.5
req POST /action -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{"op":"check","col":"wt"}'
[ "$ST" = 401 ] && ok "an action without a session is 401" || bad "action without session $ST"
req POST /session -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{"secret":"dash-enable-secret"}'
[ "$ST" = 200 ] && ok "the right secret opens a session after the lockout" || bad "right secret: $ST $(cat "$D/body")"
TOK=$(jq1 "$D/body" "d['token']")
[ "${#TOK}" -ge 64 ] && ok "a token (${#TOK} hex)" || bad "token [$TOK]"
[ "$(jq1 "$D/body" "d['members']")" = 2 ] && ok "enabled on both members" || bad "enabled on $(jq1 "$D/body" "d['members']")"
grep -q "raised privilege with enable" "$D/n1.log" && grep -q "raised privilege with enable" "$D/n2.log" && ok "both daemons logged the raised privilege" || bad "a daemon did not log the enable"
grep -q "privileged session opened" "$D/dash.log" && ok "the tool logged the session" || bad "no session log line"
req GET /session
[ "$(jq1 "$D/body" "d['enabled']")" = True ] && ok "/session says enabled" || bad "/session not enabled"

# S415: the token is a capability any instance holding the session key opens
# - a second instance (same key) acts on it without a prompt; a third (its
# own key) refuses it
sed -e "s/^listen = .*/listen = 127.0.0.1:$HP2/" "$D/dash.conf" > "$D/dash2.conf"
sed -e "s/^listen = .*/listen = 127.0.0.1:$HP3/" -e "/^session_key/d" "$D/dash.conf" > "$D/dash3.conf"
chmod 600 "$D/dash2.conf" "$D/dash3.conf"
"$DASH" -f "$D/dash2.conf" > "$D/dash2.out" 2> "$D/dash2.log" & PD2=$!
"$DASH" -f "$D/dash3.conf" > "$D/dash3.out" 2> "$D/dash3.log" & PD3=$!
i=0; while [ $i -lt 50 ] && ! { grep -q ready "$D/dash2.out" && grep -q ready "$D/dash3.out"; }; do sleep 0.1; i=$((i+1)); done
reqp() { port=$1; m=$2; p=$3; shift 3; ST=$(curl -s -o "$D/body" -D "$D/hdr" -w '%{http_code}' -X "$m" "$@" "http://127.0.0.1:$port$p"); }
A="-H X-Perfcached-Dashboard:1 -H Content-Type:application/json -H X-Perfcached-Session:$TOK"
reqp $HP2 GET /session
[ "$(jq1 "$D/body" "d['enabled']")" = False ] && ok "instance 2 holds no session of its own" || bad "instance 2 enabled before any token: $(cat "$D/body")"
reqp $HP2 POST /action $A --data '{"op":"stop","col":"wt"}'
[ "$ST" = 200 ] || [ "$ST" = 409 ] && ok "instance 2 accepts the token issued by instance 1 ($ST)" || bad "instance 2: $ST $(cat "$D/body")"
grep -q "adopted from a token" "$D/dash2.log" && ok "instance 2 adopted the secret from the token" || bad "no adoption line in instance 2's log"
reqp $HP2 GET /session
[ "$(jq1 "$D/body" "d['enabled']")" = True ] && ok "instance 2 is privileged now" || bad "instance 2 not enabled after the token"
reqp $HP3 POST /action $A --data '{"op":"stop","col":"wt"}'
[ "$ST" = 401 ] && ok "instance 3 (another key) refuses the token" || bad "instance 3: $ST"
kill -TERM $PD2 $PD3 2>/dev/null; wait $PD2 $PD3 2>/dev/null; PD2= PD3=
req POST /action -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' -H 'X-Perfcached-Session: 00000000000000000000000000000000' --data '{"op":"check","col":"wt"}'
[ "$ST" = 401 ] && ok "a wrong token is 401" || bad "wrong token $ST"

echo "--- 4. the actions"
A="-H X-Perfcached-Dashboard:1 -H Content-Type:application/json -H X-Perfcached-Session:$TOK"
req POST /action $A --data '{"op":"bogus","col":"wt"}'
[ "$ST" = 400 ] && ok "a bad op is 400" || bad "bad op $ST"
req POST /action $A --data '{"op":"check"}'
[ "$ST" = 400 ] && ok "no col is 400" || bad "no col $ST"
# check reads the upstream's master: the writer may still be finding it
i=0
while [ $i -lt 40 ]; do
	req POST /action $A --data '{"op":"check","col":"wt"}'
	[ "$ST" = 409 ] && grep -q "writer has none now" "$D/body" || break
	sleep 0.25; i=$((i+1))
done
[ "$ST" = 200 ] && ok "check: 200 $(cat "$D/body" | cut -c1-80)" || bad "check: $ST $(cat "$D/body")"
[ "$(jq1 "$D/body" "d['col']")" = wt ] && [ -n "$(jq1 "$D/body" "d['upfill']")" ] && ok "the daemon's answer came back (col, upfill)" || bad "answer shape: $(cat "$D/body")"
grep -q "check wt on node" "$D/dash.log" && ok "the action is logged with its node" || bad "no action log line"
req POST /action $A --data '{"op":"check","col":"no-such-collection"}'
[ "$ST" = 409 ] || [ "$ST" = 502 ] && ok "an unknown collection is refused by the daemon ($ST)" || bad "unknown collection $ST"
req POST /action $A --data "{\"op\":\"stop\",\"col\":\"wt\",\"node\":$OTHER}"
[ "$ST" = 200 ] || [ "$ST" = 409 ] && ok "stop on node $OTHER answered ($ST)" || bad "stop: $ST $(cat "$D/body")"
grep -q "on node $OTHER" "$D/dash.log" && ok "the action went to node $OTHER" || bad "no log line for node $OTHER"
req POST /action $A --data '{"op":"upfill","col":"wt"}'
[ "$ST" = 200 ] || [ "$ST" = 409 ] && ok "upfill answered ($ST)" || bad "upfill: $ST $(cat "$D/body")"
req POST /reset-stats $A --data '{}'
[ "$ST" = 200 ] && ok "reset-stats 200" || bad "reset-stats $ST $(cat "$D/body")"
req POST /reset-stats -H 'X-Perfcached-Dashboard: 1' -H 'Content-Type: application/json' --data '{}'
[ "$ST" = 401 ] && ok "reset-stats without a session 401" || bad "reset without session $ST"
if grep -iE "error|warning" "$D/dash.log" | grep -v "locked out\|refused" | grep -q .; then
	bad "unexpected errors in the tool's log: $(grep -iE 'error|warning' "$D/dash.log" | grep -v 'locked out\|refused' | head -2)"
else ok "no unexpected error in the tool's log"; fi

echo "--- 5. a member gone, the tool stopped"
kill -9 $PB 2>/dev/null; wait $PB 2>/dev/null; PB=
i=0
while [ $i -lt 40 ]; do
	req GET "/stats?node=$OTHER"
	[ "$ST" = 502 ] && break
	sleep 0.25; i=$((i+1))
done
[ "$ST" = 502 ] && ok "the gone member is 502: $(jq1 "$D/body" "d['error']" | cut -c1-70)" || bad "gone member: $ST"
req GET /stats
[ "$ST" = 200 ] && ok "the other member still answers" || bad "survivor $ST"
req GET /health
[ "$ST" = 200 ] && [ "$(jq1 "$D/body" "d['connected']")" = 1 ] && ok "/health 200 with one member connected" || bad "/health after one member gone: $ST $(cat "$D/body")"
# S415: no member at all -> 503, what a load balancer's check needs
kill -9 $PA 2>/dev/null; wait $PA 2>/dev/null; PA=
i=0
while [ $i -lt 40 ]; do
	req GET /health
	[ "$ST" = 503 ] && break
	sleep 0.25; i=$((i+1))
done
[ "$ST" = 503 ] && [ "$(jq1 "$D/body" "d['connected']")" = 0 ] && ok "/health 503 with no member reachable" || bad "/health with no member: $ST $(cat "$D/body")"
kill -TERM $PD
i=0; while [ $i -lt 30 ] && kill -0 $PD 2>/dev/null; do sleep 0.1; i=$((i+1)); done
kill -0 $PD 2>/dev/null && bad "the tool did not stop on TERM" || ok "TERM stops the tool"
grep -q "stopped" "$D/dash.log" && ok "the tool logged its stop" || bad "no stop line"
PD=

echo "dashtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
