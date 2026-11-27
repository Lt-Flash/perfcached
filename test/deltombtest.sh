#!/bin/sh
# deltombtest.sh - a set and its delete microseconds apart on one connection leave the key on no
# member (the tombstone carries the delete's version); both delete paths.  Usage: test/deltombtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
SEC=dt-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcdt.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill "$p"; done; pkill -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

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
# perfcli's own complaint is kept: an err that survives the retries is reported with it
cli() { timeout 30 ./perfcli -h 127.0.51.$1 -p 1798$1 -a $SEC -q 2>"$D/cli.err.$1"; }
# exists, not get: under redis_types = strict a get of a JSON document is WRONGTYPE, a false green
found1() {
	printf '%s\n' "{\"method\":\"exists\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}" | cli "$1" | head -1 | tee "$D/cli.out.$1" |
		python3 -c 'import json,sys
try:
    r = json.load(sys.stdin)
    print("err" if "error" in r else "yes" if r.get("exists") else "no")
except Exception: print("err")'
}
# a fresh perfcli connection right behind another can be refused for a moment: retry an err first
found() { # found <node> <key> -> yes|no|err
	_r=$(found1 "$1" "$2"); _t=0
	while [ "$_r" = err ] && [ $_t -lt 3 ]; do sleep 0.1; _r=$(found1 "$1" "$2"); _t=$((_t+1)); done
	[ "$_r" = err ] && _r="err[$(head -c 100 "$D/cli.out.$1" | tr -d '\n') | $(tail -1 "$D/cli.err.$1" 2>/dev/null | cut -c1-120)]"
	printf '%s' "$_r"
}
peers_up() { printf '%s\n' '{"method":"stats"}' | cli "$1" | head -1 | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up", -1))
except Exception: print(-1)'; }
# READY, not only peered: a node that joined holding nothing reports recovering for ~2 s and
# refuses probes
upready() { printf '%s\n' '{"method":"stats"}' | cli "$1" | head -1 | python3 -c 'import json,sys
try: r = json.load(sys.stdin); print("%s:%s" % ((r.get("cluster") or {}).get("peers_up", -1), r.get("state")))
except Exception: print(-1)'; }

for i in 1 2 3; do mk $i; done
for i in 1 2 3; do start $i || exit 1; done
i=0
while [ $i -lt 100 ]; do
	[ "$(upready 1)" = 2:ready ] && [ "$(upready 2)" = 2:ready ] && [ "$(upready 3)" = 2:ready ] && break
	sleep 0.2; i=$((i + 1))
done
[ "$(peers_up 1)" = 2 ] && ok "three eager members formed (peers_up 2 on node 1)" || { bad "the fleet did not form"; exit 1; }

# the pair on ONE connection, no delay: both lines in one perfcli run
pair() { # pair <node> <key> <del-json> [write-json]
	_w=${4:-}
	[ -n "$_w" ] || _w="{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"$2\",\"value\":\"v\"}}"
	printf '%s\n%s\n' "$_w" "$3" | cli "$1" >/dev/null
}
# every member must say absent at every sample across 2.5 s: the batch flushes at 3 ms, so a
# resurrection shows by 100 ms
absent_everywhere() { # absent_everywhere <key> -> "" or the offending samples
	_bad=""
	for t in 1 2 3 4 5; do
		sleep 0.5
		for n in 1 2 3; do
			r=$(found $n "$1")
			[ "$r" = no ] || _bad="$_bad n$n@$((t * 500))ms=$r"
		done
	done
	printf '%s' "$_bad"
}

pair 1 "pd" '{"method":"del","params":{"col":"c","key":"pd"}}'
r=$(absent_everywhere pd)
[ -z "$r" ] && ok "set then del on one connection: the key is absent on all three members, every sample" \
	|| bad "set then del: the key came back somewhere ($r)"

# a document, then its root delete (a root jdel refuses a plain string under strict, as Redis's
# JSON.DEL does)
pair 1 "pj" '{"method":"jdel","params":{"col":"c","key":"pj","path":"$"}}' \
	'{"method":"jset","params":{"col":"c","key":"pj","path":"$","val":{"s":"v"}}}'
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

# on a failure, what the three cluster planes did: only the node logs say why one rejoined
if [ $fail -gt 0 ]; then
	for i in 1 2 3; do
		echo "  --- n$i: cluster / state lines"
		grep -a -E "cluster:|node state|joined|founding|elect|master|term" "$D/n$i.log" 2>/dev/null | tail -25 | cut -c1-220 | sed 's/^/    /'
	done
fi

echo "deltombtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
