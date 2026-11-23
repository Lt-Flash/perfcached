#!/bin/sh
# upfaulttest.sh - an upstream that answers out of the ordinary: errors, odd COMMAND INFO numbers, late
# replies.  Collection 0 writes through to fake master M (read from replica R); collection 1 (RESP db 1)
# reads through from fake U alone.  Usage: test/upfaulttest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupf.XXXXXX)
PM=16631 PR=16632 PU=16633 TCP=16634 RESP=16635 HTTP=16636
PID= FM= FR= FU=
trap 'kill -9 $PID 2>/dev/null; for p in $FM $FR $FU; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $PR $PU $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upfaulttest: port $p already bound" >&2; exit 1; }
done
for n in m r u; do mkdir -p "$D/up$n"; done
cat > "$D/upm/data.json" <<'J'
{"wt:j": {"type": "json", "value": {"o": "a"}}}
J
cp "$D/upm/data.json" "$D/upr/data.json"
cat > "$D/upu/data.json" <<'J'
{"rd:1": {"type": "string", "value": "v1"},
 "dr:1": {"type": "string", "value": "d1"},
 "dr:2": {"type": "string", "value": "d2"},
 "dr:3": {"type": "string", "value": "d3"}}
J
CM='"role": "master", "accept_writes": true, "tracking": false, "replicate_to": ["'"$D"'/upr"]'
CR='"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM', "tracking": false'
ctl() { # ctl <m|r|u> <json body, without braces>
	echo "{$2}" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"
}
ctl m "$CM"; ctl r "$CR"; ctl u '"resp3": true'
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
python3 "$T/fakeupstream.py" $PR "$D/upr" > "$D/fr.log" 2>&1 & FR=$!
python3 "$T/fakeupstream.py" $PU "$D/upu" > "$D/fu.log" 2>&1 & FU=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/upr/ready" ] && [ -f "$D/upu/ready" ]; }; do
	sleep 0.1; i=$((i+1)); done
mkdir -p "$D/state"
cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = upf-client-secret
enable = upf-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
resp_collections = 0, 1
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$PR, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 2000
[collection 1]
buckets_log2 = 10
upstream = redis://127.0.0.1:$PU
upstream_prefixes = rd:*, dr:*
upstream_timeout_ms = 1000
upstream_negative_ms = 2000
C
chmod 600 "$D/n.conf"
start() { # start [VAR=value ...]: the node, its log $D/n.log
	: > "$D/n.log"
	env "$@" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n.log"
}
start || { bad "the node did not start: $(tail -3 "$D/n.log")"; echo "upfaulttest: $pass passed, $fail failed"; exit 1; }
wt() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
w=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]["write_through"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done
# rc <db> <cmd> [<cmd> ...]: one connection, SELECT <db>, the commands pipelined; a command's
# arguments split at '|'; one line per reply (decoded, as Python literals)
cat > "$D/rc.py" <<'P'
import socket, sys
def enc(parts):
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p) for p in parts)
def rd(f):
    l = f.readline()
    t, rest = l[:1], l[1:-2]
    if t in (b"+", b"-"): return (t + rest).decode()
    if t == b":": return int(rest)
    if t == b"_": return None
    if t == b"$":
        n = int(rest)
        return None if n < 0 else f.read(n + 2)[:-2].decode("latin-1")
    if t == b"*":
        n = int(rest)
        return None if n < 0 else [rd(f) for _ in range(n)]
    return "?" + l.decode("latin-1")
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 20)
cmds = [[b"SELECT", sys.argv[2].encode()]] + [c.encode("latin-1").split(b"|") for c in sys.argv[3:]]
s.sendall(b"".join(enc(c) for c in cmds))
f = s.makefile("rb")
rd(f)
for _ in sys.argv[3:]:
    print(repr(rd(f)))
P
rc() { python3 "$D/rc.py" $RESP "$@" 2>&1; }
# a crashed node stays a zombie until waited for: kill -0 alone would call it alive
alive() { kill -0 $PID 2>/dev/null && ! grep -q '^State:[[:space:]]*Z' /proc/$PID/status 2>/dev/null; }

echo "--- 1: COMMAND INFO's key step from the upstream"
# first 1, last -1, step INT_MAX: the step past the last argument must end the key walk
ctl m "$CM"', "cmds": {"xstep": [-3, ["write"], 1, -1, 2147483647, ["@write"]]}'
sleep 0.2
R=$(rc 0 'XSTEP|st:1|v')
sleep 0.3
alive && [ -n "$R" ] && ok "1. XSTEP (step 2147483647) answered ($R), the node still up" \
	|| bad "1. XSTEP with step 2147483647: the node went down ($R; $(tail -2 "$D/n.log" | tr '\n' ' '))"
alive || { echo "upfaulttest: $pass passed, $fail failed"; exit 1; }
ctl m "$CM"

echo "--- 2: a sub-command's COMMAND INFO answered late"
# xmov has movable keys, so its "xmov|sub" is asked too - after the timeout (1000 ms): that reply is
# still coming, and the connection must not be used on for the next write
ctl m "$CM"', "cmds": {"xmov": [-2, ["write", "movablekeys"], 0, 0, 0, ["@write"]], "xmov|sub": [-2, ["write"], 2, 2, 1, ["@write"]]}, "cmd_info_delay_ms": {"xmov|sub": 1600}'
sleep 0.2
R1=$(rc 0 'XMOV|sub|mv:1')
sleep 1
R2=$(rc 0 'SET|mv:2|v2')
R3=$(rc 0 'SET|mv:3|v3' 'GET|mv:3' | tr '\n' ' ')
[ "$R2" = "'+OK'" ] && [ "$R3" = "'+OK' 'v3' " ] \
	&& ok "2. the writes after it answered as themselves ($R2; $R3) - XMOV: $R1" \
	|| bad "2. after a late COMMAND INFO the next writes read other replies: SET $R2, SET+GET $R3 (XMOV: $R1)"
ctl m "$CM"

echo "--- 3: DEL of 100 keys, written through"
# no tracking here (the fakes refuse it): the fleet's copies go only by the DEL's read-back
SETS=""; KEYS=""
for i in $(seq 1 100); do SETS="$SETS SET|dk:$i|v$i"; KEYS="$KEYS|dk:$i"; done
R=$(rc 0 $SETS | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
G=$(rc 0 'GET|dk:1' 'GET|dk:100' | tr '\n' ' ')
[ "$G" = "'v1' 'v100' " ] || bad "3. setup: 100 SETs ($R), GET dk:1 dk:100: $G"
R=$(rc 0 "DEL$KEYS")
G=$(rc 0 'GET|dk:1' 'GET|dk:64' 'GET|dk:65' 'GET|dk:100' | tr '\n' ' ')
[ "$R" = "100" ] && [ "$G" = "None None None None " ] \
	&& ok "3. DEL of 100 keys: Redis deleted 100, the fleet answers nil for every one" \
	|| bad "3. DEL of 100 keys ($R): GET dk:1 dk:64 dk:65 dk:100 = $G - keys past the 64th still served"

echo "--- 4: a declined JSON.SET ... NX EX keeps the key's expiry at Redis"
# wt:j exists without a TTL: NX declines, and its EXPIRE must not be sent after it
R=$(rc 0 'JSON.SET|wt:j|$|{"o":"b"}|NX|EX|30')
sleep 0.3
X=$(python3 -c 'import json,sys; e=json.load(open(sys.argv[1]))["wt:j"]; print(e["value"]["o"], "exp_ms" in e)' "$D/upm/data.json" 2>&1)
E=$(grep -c "^EXPIRE wt:j" "$D/upm/writes" 2>/dev/null)
[ "$R" = None ] && [ "$X" = "a False" ] && [ "${E:-0}" = 0 ] \
	&& ok "4. JSON.SET NX EX on an existing document: nil, and Redis's copy kept as it was (no TTL, no EXPIRE sent)" \
	|| bad "4. JSON.SET NX EX declined ($R): Redis now value/expiry '$X', EXPIRE sent ${E:-0} time(s)"

echo "--- 5: an error reply to TYPE is a failed fetch, not a type"
# Redis loading its dataset answers TYPE -LOADING; collection 1's read must fail (on_error = error),
# and nothing may be remembered of it once the upstream answers again
ctl u '"resp3": true, "error_cmds": {"TYPE": "LOADING Redis is loading the dataset in memory"}'
sleep 0.2
R=$(rc 1 'GET|rd:1')
case "$R" in *WRONGTYPE*|"'v1'"|None) bad "5. TYPE answered -LOADING: GET rd:1 = $R (want an upstream error)";;
	"'-"*|-*) ok "5. TYPE answered -LOADING: GET rd:1 is an error ($R)";;
	*) bad "5. TYPE answered -LOADING: GET rd:1 = $R";; esac
ctl u '"resp3": true'
sleep 0.2
R=$(rc 1 'GET|rd:1')
[ "$R" = "'v1'" ] && ok "5. the upstream back: rd:1 reads at once ('v1')" \
	|| bad "5. the upstream back: GET rd:1 = $R (a marker kept from the -LOADING)"

echo "--- 6: the drain sweep's first SCAN page refused once"
dr() { python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),10))
pcnative.call(f,"enable",secret="upf-enable-secret")
r=pcnative.call(f,"updrain",col="1")
print(json.dumps(r.get("result", r.get("error"))))' $TCP 2>&1; }
dstat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
d=[c for c in r["collections"] if c["name"]=="1"][0]["upstream"]["drain"]
print(d[sys.argv[1]])' "$1" 2>/dev/null; }
drwait() { k=0; while [ "$(dstat state)" = running ] && [ $k -lt 150 ]; do sleep 0.2; k=$((k+1)); done; }
ctl u '"resp3": true, "error_cmds": {"SCAN": "ERR busy, try again"}, "error_n": 1, "error_seq": 6'
sleep 0.2
R=$(dr)
drwait
ST="$(dstat state) read=$(dstat read) taken=$(dstat taken_over) errors=$(dstat errors)"
# rd:1 and dr:1..3 under two patterns: the refused page was the first pattern's first
[ "$(dstat state)" = done ] && [ "$(dstat read)" -ge 4 ] 2>/dev/null \
	&& ok "6. one refused SCAN is retried: the sweep reads every pattern ($ST)" \
	|| bad "6. a refused first SCAN page skipped the pattern: $ST (updrain: $R; $(grep -c '^SCAN' "$D/upu/counts") SCANs)"
ctl u '"resp3": true'

echo "--- 7: a value over the sweep's 1 MiB reply ceiling"
python3 -c 'import json,sys
d=json.load(open(sys.argv[1])); d["dr:big"]={"type":"string","value":"x"*1500000}; d["dr:4"]={"type":"string","value":"d4"}
open(sys.argv[1]+".tmp","w").write(json.dumps(d))' "$D/upu/data.json" && mv "$D/upu/data.json.tmp" "$D/upu/data.json"
sleep 0.2
R=$(dr)
drwait
ST="$(dstat state) read=$(dstat read) too_big=$(dstat too_big) errors=$(dstat errors)"
G=$(rc 1 'GET|dr:4')
[ "$(dstat state)" = done ] && [ "$(dstat too_big)" -ge 1 ] 2>/dev/null && [ "$G" = "'d4'" ] \
	&& ok "7. the 1.5 MB value is counted too big and passed over, the rest of its page swept ($ST)" \
	|| bad "7. one value over 1 MiB: $ST, dr:4 = $G (updrain: $R)"

echo "--- 8: a stop while the drain sweep waits for its posts"
# PERFCACHED_TEST_DRAIN_UNAPPLIED: one post no worker applies, as when the workers stopped before the
# sweep's end; the stop must still end the sweep, and the node exit
kill $PID; wait $PID 2>/dev/null
start PERFCACHED_TEST_DRAIN_UNAPPLIED=1 || bad "8. the node did not restart: $(tail -3 "$D/n.log")"
R=$(dr)
# its keys read, the sweep waits on at its end: until then it is running, whatever it has read
k=0 last=-1 same=0
while [ $k -lt 75 ] && [ "$(dstat state)" = running ] && [ $same -lt 10 ]; do
	n=$(dstat read); [ "$n" = "$last" ] && same=$((same+1)) || same=0; last=$n
	sleep 0.2; k=$((k+1))
done
sleep 0.5
R2=$(dr)
case "$R2" in *"already running"*) ok "8. a second updrain while the sweep waits for its posts: refused, already running ($(dstat state))";;
	*) bad "8. a second updrain while the sweep waits for its posts: $(echo "$R2" | tail -1) (state $(dstat state); want 'already running', not a worker joined to the old sweep)";; esac
kill $PID
k=0; while alive && [ $k -lt 100 ]; do sleep 0.2; k=$((k+1)); done
if alive; then
	bad "8. SIGTERM during the sweep's wait: the node still runs after 20 s (state $(dstat state); updrain: $R)"
	kill -9 $PID 2>/dev/null
else
	ok "8. SIGTERM during the sweep's wait: the node exited"
fi
wait $PID 2>/dev/null; PID=
start || { bad "the node did not restart after 8: $(tail -3 "$D/n.log")"; echo "upfaulttest: $pass passed, $fail failed"; exit 1; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done

echo "--- 9: the unknown commands' classes - movable keys without sub-commands, a full cache"
# xmk and xmk2 have movable keys and no sub-commands: their first argument is a key, never a
# sub-command to ask; past 128 names cached, a new command's class is not lost to its own lookup
ctl m "$CM"', "cmds": {"xmk": [-2, ["write", "movablekeys"], 1, 1, 1, ["@write"]], "xmk2": [-2, ["write", "movablekeys"], 1, 1, 1, ["@write"]]}'
sleep 0.2
R=$(rc 0 'XMK|mk:1|v' 'XMK|mk:2|v' | tr '\n' ' ')
N=$(grep -c '^COMMAND INFO xmk|' "$D/upm/counts")
[ "$N" = 0 ] && ok "9. XMK twice: no 'xmk|<key>' asked as a sub-command ($R)" \
	|| bad "9. XMK twice: 'xmk|<key>' asked $N time(s) as a sub-command ($R)"
B=""; for i in $(seq 1 130); do B="$B NOCMD$i|x"; done
U=$(rc 0 $B | grep -c 'unknown command')
R=$(rc 0 'XMK2|mk:3|v')
case "$R" in *"refuses writes"*) ok "9. after $U unknown names filled the class cache, XMK2 is still passed to the master ($R)";;
	*) bad "9. after $U unknown names filled the class cache, XMK2: $R (want the master's own answer)";; esac
ctl m "$CM"

echo "--- 10: a write whose read-back is over the reply ceiling"
# the write is applied at Redis: its answer is the master's, and the fleet keeps no older copy
R=$(rc 0 'SET|bg:a|small' 'GET|bg:a' | tr '\n' ' ')
[ "$R" = "'+OK' 'small' " ] || bad "10. setup: SET + GET bg:a = $R"
python3 -c 'import json, os, sys
for d in sys.argv[1:]:
    p = d + "/data.json"; x = json.load(open(p)); x["bg:a"] = {"type": "string", "value": "x" * 1100000}
    open(p + ".tmp", "w").write(json.dumps(x)); os.rename(p + ".tmp", p)' "$D/upm" "$D/upr"
sleep 0.3
R=$(rc 0 'APPEND|bg:a|y')
G=$(rc 0 'GET|bg:a' | cut -c 1-40)
[ "$R" = 1100001 ] && [ "$G" != "'small'" ] \
	&& ok "10. APPEND making bg:a 1.1 MB at Redis: the master's 1100001, the fleet's older copy dropped ($G)" \
	|| bad "10. APPEND making bg:a 1.1 MB at Redis: $R, GET bg:a then $G (want 1100001, not 'small')"
# upstream_pass_max_kb 64 bounds what is passed, not a record's read-back
kill $PID; wait $PID 2>/dev/null; PID=
sed -i 's/^upstream_writes = through$/&\nupstream_pass_max_kb = 64/' "$D/n.conf"
start || { bad "10. the node did not restart: $(tail -3 "$D/n.log")"; echo "upfaulttest: $pass passed, $fail failed"; exit 1; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done
V=$(head -c 100000 /dev/zero | tr '\0' v)
R=$(rc 0 "SET|bg:c|$V")
sleep 1                                # the read-back's record can trail the +OK on a loaded runner
N0=$(grep -c '^GET bg:c' "$D/upr/counts")
L=$(rc 0 'GET|bg:c' | wc -c)
N1=$(grep -c '^GET bg:c' "$D/upr/counts")
[ "$R" = "'+OK'" ] && [ "$L" = 100003 ] && [ "$N1" = "$N0" ] \
	&& ok "10. upstream_pass_max_kb 64: a 100 KB SET answered +OK and its read-back held, GET served here" \
	|| bad "10. upstream_pass_max_kb 64, a 100 KB SET: $(echo "$R" | cut -c 1-80), GET $L bytes, upstream GETs $N0 -> $N1"

echo "--- 11: an unknown command naming more than 64 keys"
# collection 0 passes xm:* alone: XMSET's 65th key lies outside, and every key counts
kill $PID; wait $PID 2>/dev/null; PID=
sed -i 's/^upstream_prefixes = \*$/upstream_prefixes = xm:*/' "$D/n.conf"
start || { bad "11. the node did not restart: $(tail -3 "$D/n.log")"; echo "upfaulttest: $pass passed, $fail failed"; exit 1; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done
ctl m "$CM"', "cmds": {"xmset": [-3, ["write"], 1, -1, 2, ["@write"]]}'
sleep 0.2
A="XMSET"; for i in $(seq 1 64); do A="$A|xm:$i|v"; done; A="$A|out:65|v"
R=$(rc 0 "$A")
W=$(grep -c '^XMSET' "$D/upm/writes" 2>/dev/null)
case "$R" in *"outside upstream_prefixes"*) [ "${W:-0}" = 0 ] && ok "11. XMSET with its 65th key outside upstream_prefixes: refused, nothing sent ($R)" \
		|| bad "11. XMSET refused ($R) but sent to the master $W time(s)";;
	*) bad "11. XMSET with its 65th key outside upstream_prefixes: $R, sent to the master ${W:-0} time(s)";; esac
ctl m "$CM"

echo "--- 12: plain SETs of new keys, far faster than the upstream answers"
# a plain SET does not wait for its ownership check; past 4096 checks queued the key is taken as
# the fleet's at once (a no-fall entry), not queued without a bound
ust() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
print([c for c in s["collections"] if c["name"]=="1"][0]["upstream"][sys.argv[1]])' "$1" 2>/dev/null; }
ctl u '"resp3": true, "delay_ms": 20000'
sleep 0.2
O0=$(ust ownership_checks); W0=$(ust nofall_written)
B=""; for i in $(seq 1 5000); do B="$B SET|rd:o$i|v"; done
R=$(rc 1 $B | grep -c OK)
O1=$(ust ownership_checks); W1=$(ust nofall_written)
if [ -n "$O0" ] && [ -n "$O1" ] && [ "$R" = 5000 ] && [ $((O1 - O0)) -le 4200 ] && [ $((W1 - W0)) -ge 700 ]; then
	ok "12. 5000 plain SETs: $((O1 - O0)) ownership checks queued, $((W1 - W0)) keys taken as the fleet's at once"
else
	bad "12. 5000 plain SETs ($R OK): $((O1 - O0)) ownership checks queued, $((W1 - W0)) no-fall entries written (want at most ~4096 queued, the rest owned at once)"
fi
ctl u '"resp3": true'

echo "upfaulttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
