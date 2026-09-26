#!/bin/sh
# deltombtest.sh - S209: a write and a delete microseconds apart do not
# leave the key on the peers.
#
# The write-path push batches for up to REPL_FLUSH_MS (3 ms) while a
# tombstone goes at once, so a set and its delete closer than the flush
# reach the peers in the WRONG order: the tombstone removes nothing, the
# batched set lands after it, and the key resurrects on every peer - for
# good, since the sweep repairs presence, not absence.  Found gating the
# staging RGSs, whose de-registration is exactly set-then-delete.
#
# The tombstone now carries the delete's version and the peers refuse a
# copy that is not newer than it (S209, fix B).  This suite drives the
# pair through ONE connection with no delay - the shape that hid it from
# perfcli, whose invocations are seconds apart - and asserts on EVERY
# member, never only on the node the client held.  Both delete paths,
# the plain del and the root jdel.  A pair seconds apart stays correct,
# and a set alone still replicates (the positive control).
# Usage: test/deltombtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
SEC=dt-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcdt.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill "$p"; done; pkill -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT INT TERM

mk() { # mk <node>
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = dt-cluster-secret
[listen]
tcp = 127.0.51.$1:1798$1
[cluster]
multicast = 239.255.77.158:17267
advertise = 127.0.51.$1
tombstone_ms = 2000
[collection c]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.log"; return 1
}
cli() { timeout 30 ./perfcli -h 127.0.51.$1 -p 1798$1 -a $SEC -q 2>/dev/null; }
found1() {
	printf '%s\n' "{\"method\":\"get\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}" | cli "$1" | head -1 |
		python3 -c 'import json,sys
try: print("yes" if json.load(sys.stdin).get("found") else "no")
except Exception: print("err")'
}
# a fresh perfcli connection right behind another can be refused for a
# moment; that is the CLIENT, so an err is asked again before it counts
found() { # found <node> <key> -> yes|no|err
	_r=$(found1 "$1" "$2"); _t=0
	while [ "$_r" = err ] && [ $_t -lt 3 ]; do sleep 0.1; _r=$(found1 "$1" "$2"); _t=$((_t+1)); done
	printf '%s' "$_r"
}
peers_up() { printf '%s\n' '{"method":"stats"}' | cli "$1" | head -1 | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up", -1))
except Exception: print(-1)'; }

for i in 1 2 3; do mk $i; done
for i in 1 2 3; do start $i || exit 1; done
i=0
while [ $i -lt 100 ]; do
	[ "$(peers_up 1)" = 2 ] && [ "$(peers_up 2)" = 2 ] && [ "$(peers_up 3)" = 2 ] && break
	sleep 0.2; i=$((i + 1))
done
[ "$(peers_up 1)" = 2 ] && ok "three eager members formed (peers_up 2 on node 1)" || { bad "the fleet did not form"; exit 1; }

# the pair on ONE connection, no delay: both lines in one perfcli run
pair() { # pair <node> <key> <del-json>
	printf '%s\n%s\n' \
		"{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"$2\",\"value\":\"v\"}}" \
		"$3" | cli "$1" >/dev/null
}
# every member must say absent at every sample across 2.5 s - the batch
# flushes at 3 ms, the apply follows, so a resurrection shows by 100 ms
absent_everywhere() { # absent_everywhere <key> -> "" or the offending samples
	_bad=""
	for t in 1 2 3 4 5; do
		sleep 0.5
		for n in 1 2 3; do
			r=$(found $n "$1")
			[ "$r" = no ] || _bad="$_bad n$n@${t}00ms=$r"
		done
	done
	printf '%s' "$_bad"
}

pair 1 "pd" '{"method":"del","params":{"col":"c","key":"pd"}}'
r=$(absent_everywhere pd)
[ -z "$r" ] && ok "set then del on one connection: the key is absent on all three members, every sample" \
	|| bad "set then del: the key came back somewhere ($r)"

pair 1 "pj" '{"method":"jdel","params":{"col":"c","key":"pj","path":"$"}}'
r=$(absent_everywhere pj)
[ -z "$r" ] && ok "set then root jdel on one connection: absent on all three, every sample" \
	|| bad "set then root jdel: the key came back somewhere ($r)"

# the same pair a second apart is unaffected
printf '%s\n' '{"method":"set","params":{"col":"c","key":"ps","value":"v"}}' | cli 1 >/dev/null
sleep 1
printf '%s\n' '{"method":"del","params":{"col":"c","key":"ps"}}' | cli 1 >/dev/null
sleep 0.5
r=""; for n in 1 2 3; do [ "$(found $n ps)" = no ] || r="$r n$n"; done
[ -z "$r" ] && ok "the pair a second apart is deleted everywhere too" || bad "the slow pair left the key on$r"

# positive control: a set alone replicates, so 'absent' above is not 'never arrived'
printf '%s\n' '{"method":"set","params":{"col":"c","key":"pk","value":"v"}}' | cli 1 >/dev/null
i=0; while [ $i -lt 20 ]; do [ "$(found 2 pk)" = yes ] && [ "$(found 3 pk)" = yes ] && break; sleep 0.1; i=$((i+1)); done
[ "$(found 2 pk)" = yes ] && [ "$(found 3 pk)" = yes ] && ok "positive control: a set alone reaches both peers ($((i*100)) ms)" \
	|| bad "positive control failed: a plain set did not replicate"

# the counter names what happened: on the unfixed daemon it does not exist
tomb=$(printf '%s\n' '{"method":"stats"}' | cli 2 | head -1 | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("recv_tombstoned", "absent"))
except Exception: print("err")')
[ "$tomb" != absent ] && [ "$tomb" != err ] && [ "$tomb" -ge 1 ] 2>/dev/null \
	&& ok "a peer counted the refused set as tombstoned (recv_tombstoned=$tomb)" \
	|| bad "no peer reports a refused-as-tombstoned copy (recv_tombstoned=$tomb)"

echo "deltombtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
