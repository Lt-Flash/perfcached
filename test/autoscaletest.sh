#!/bin/sh
# autoscaletest.sh - S239: a collection's autoscale mode - auto, warn, off.
#
# On 245-247 an operator's `resize 9 17` was shrunk back to 2^12 58 s
# later by S150's auto-shrink, and nothing could say "leave this one".
# The operator's design, after Ceph's pg_autoscale_mode: AUTO grows and
# shrinks by key count (today's behaviour, the default); WARN changes
# nothing and says what auto would do; OFF changes nothing and says
# nothing.  Under WARN and OFF growth is blocked too - keys past the
# slots go to the overflow leg, which the page shows red.
#
# One node, shrink_cooloff_s = 2, six collections:
#   a, w, o  - made at 2^17 holding 200 keys (a shrink case): only a
#              (auto) shrinks; w logs what auto would do, once; o is
#              silent.  All three publish the target.
#   ga,gw,go - made at 2^12 holding 30,000 keys (past 75% of the slots):
#              only ga grows; gw and go keep 4,096 buckets and put the
#              rest in the leg; gw logs a grow recommendation.
# Then /stats and /metrics carry the mode and the target; the verb
# (privileged) sets a mode - o to auto shrinks it - and the verb's mode
# survives a restart over the config's; and a bad value is refused.
# FAIL-FIRST: the build before S239 does not know the key and will not
# start with this config.
# Usage: test/autoscaletest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcas.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18551 HPORT=18552
ss -ltn 2>/dev/null | grep -qE ":1855[12][[:space:]]" && { echo "autoscaletest: port busy" >&2; exit 1; }
mkdir -p "$D/state"
conf() { # conf <extra collection line for w>
	cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
shrink_cooloff_s = 2
[memory]
arena_mb = 128
[secrets]
client = as-client
enable = as-enable
[listen]
tcp = 127.0.0.1:$PORT
http = 127.0.0.1:$HPORT
plaintext = loopback
[collection a]
buckets_log2 = 17
[collection w]
buckets_log2 = 17
autoscale = $1
[collection o]
buckets_log2 = 17
autoscale = off
[collection ga]
buckets_log2 = 12
[collection gw]
buckets_log2 = 12
autoscale = warn
[collection go]
buckets_log2 = 12
autoscale = off
EOF
	chmod 600 "$D/n.conf"
}
start() {
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && return 0; kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	return 1
}
col() { # col <name> -> "buckets overflow autoscale target"
	curl -s "http://127.0.0.1:$HPORT/stats" | python3 -c '
import json, sys
c = [c for c in json.load(sys.stdin)["collections"] if c["name"] == sys.argv[1]]
c = c[0] if c else {}
print(c.get("buckets"), c.get("overflow"), c.get("autoscale", "-"), c.get("autoscale_target_log2", "-"))' "$1"; }
w() { echo "$1" | cut -d' ' -f"$2"; }
fill() { python3 - "$PORT" "$1" "$2" <<'PY'
import json, socket, sys
port, c, n = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", port), timeout=30); f = s.makefile("rb")
for b in range(0, n, 200):
    s.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": c, "key": "k%06d" % i, "value": "v"}}) + "\n" for i in range(b, min(n, b + 200))).encode())
    for _ in range(b, min(n, b + 200)): f.readline()
PY
}

conf warn
start || { echo "autoscaletest: did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"; bad "the node did not start with autoscale in its config"; echo "autoscaletest: $pass passed, $fail failed"; exit 1; }
for c in a w o; do fill $c 200; done
for c in ga gw go; do fill $c 30000; done
sleep 10
A=$(col a); W=$(col w); O=$(col o); GA=$(col ga); GW=$(col gw); GO=$(col go)
echo "   a: $A | w: $W | o: $O"
echo "   ga: $GA | gw: $GW | go: $GO"
[ "$(w "$A" 1)" = 4096 ] && [ "$(w "$W" 1)" = 131072 ] && [ "$(w "$O" 1)" = 131072 ] \
	&& ok "200 keys in 2^17: auto shrank to 4,096 buckets; warn and off kept 131,072" \
	|| bad "shrink: a $(w "$A" 1), w $(w "$W" 1), o $(w "$O" 1) (want 4096, 131072, 131072)"
[ "$(w "$W" 4)" = 12 ] && [ "$(w "$O" 4)" = 12 ] && [ "$(w "$W" 3)" = warn ] && [ "$(w "$O" 3)" = off ] \
	&& ok "/stats says the mode and what auto would pick (2^12) for warn and off alike" \
	|| bad "stats: w '$W', o '$O' (want warn/off with target 12)"
NW=$(grep -c "collection 'w' (autoscale = warn).*auto would shrink it to 2^12" "$D/n.log")
NO=$(grep -c "collection 'o' (autoscale" "$D/n.log")
[ "$NW" = 1 ] && [ "$NO" = 0 ] \
	&& ok "warn said it once - $(grep -m1 "collection 'w' (autoscale" "$D/n.log" | sed 's/.*WARNING: //' | cut -c1-100)... - off said nothing" \
	|| bad "log: warn said it $NW time(s) (want 1), off $NO (want 0)"
[ "$(w "$GA" 1)" -gt 4096 ] 2>/dev/null && [ "$(w "$GW" 1)" = 4096 ] && [ "$(w "$GO" 1)" = 4096 ] \
	&& [ "$(w "$GW" 2)" -gt 0 ] 2>/dev/null && [ "$(w "$GO" 2)" -gt 0 ] 2>/dev/null \
	&& ok "30,000 keys in 2^12: auto grew to $(w "$GA" 1) buckets; warn and off stayed at 4,096 with $(w "$GW" 2) / $(w "$GO" 2) in the leg" \
	|| bad "growth: ga $(w "$GA" 1), gw $(w "$GW" 1)/$(w "$GW" 2) leg, go $(w "$GO" 1)/$(w "$GO" 2) leg"
grep -q "collection 'gw' (autoscale = warn).*growth blocked.*auto would grow it to 2^" "$D/n.log" \
	&& ok "and warn recommended the growth it blocked" || bad "gw: no grow recommendation in the log"
# the operator: "in off mode collection should also turn red when there's
# too many keys" - off says nothing in the log, but publishes the target
# the page turns red on (auto would grow it: target above its size)
[ "$(w "$GO" 4)" -gt 12 ] 2>/dev/null && ! grep -q "collection 'go' (autoscale" "$D/n.log" \
	&& ok "off published its grow target (2^$(w "$GO" 4) over 4,096 buckets: the page's red) and logged nothing" \
	|| bad "go: target '$(w "$GO" 4)' (want > 12) or it logged"
M=$(curl -s "http://127.0.0.1:$HPORT/metrics")
echo "$M" | grep -q 'perfcached_collection_autoscale_mode{collection="w",mode="warn"} 1' \
	&& echo "$M" | grep -q 'perfcached_collection_autoscale_target_buckets{collection="w"} 4096' \
	&& ok "/metrics: the mode gauge and the target (4096) for w" || bad "/metrics lacks the autoscale series"

R=$("$CLI" -q -h 127.0.0.1 -p $PORT autoscale o auto 2>&1)
case "$R" in *rivileg*|*enable*) ok "the verb needs privilege: $(echo "$R" | cut -c1-60)...";; *) bad "unprivileged autoscale was not refused: $R";; esac
R=$("$CLI" -q -h 127.0.0.1 -p $PORT -E as-enable autoscale o auto 2>&1)
sleep 6
O=$(col o)
[ "$(w "$O" 3)" = auto ] && [ "$(w "$O" 1)" = 4096 ] \
	&& ok "autoscale o auto: the mode changed and the table shrank ($R)" \
	|| bad "after the verb: o '$O' (want auto at 4096) - $R"
"$CLI" -q -h 127.0.0.1 -p $PORT -E as-enable autoscale a off >/dev/null 2>&1
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
start || { bad "the node did not restart"; echo "autoscaletest: $pass passed, $fail failed"; exit 1; }
[ "$(w "$(col a)" 3)" = off ] && [ "$(w "$(col o)" 3)" = auto ] \
	&& ok "after a restart the verb's modes held over the config's: a off, o auto" \
	|| bad "after a restart: a '$(col a)', o '$(col o)' (want off, auto)"
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
conf maybe
start && bad "autoscale = maybe was accepted" \
	|| { grep -q "autoscale: auto|warn|off" "$D/n.log" && ok "a bad value is refused: $(grep -m1 "autoscale:" "$D/n.log" | sed 's/.*ERROR: //' | cut -c1-80)" \
		|| bad "a bad value did not name the choices: $(tail -2 "$D/n.log" | tr '\n' ' ')"; }
echo "autoscaletest: $pass passed, $fail failed"
[ $fail -eq 0 ]
