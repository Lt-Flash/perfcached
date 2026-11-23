#!/bin/sh
# upreadplacetest.sh - a READ-ONLY read-through collection in shard and in spread fetches a key at
# the node it belongs to, as a written-through one does, and nowhere else.
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcuprp.XXXXXX)
UP=20610
FPID= P1ID= P2ID= P3ID=
trap 'for p in $P1ID $P2ID $P3ID $FPID; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $UP 20619 20621 20622 20623 20629 20631 20632 20633 20641 20642 20643; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upreadplacetest: port $p already bound" >&2; exit 1; }
done
mkdir -p "$D/up"
python3 -c 'import json,sys; json.dump({"ro:k%d" % i: {"type": "string", "value": "v%d" % i} for i in range(1, 31)}, open(sys.argv[1], "w"))' "$D/up/data.json"
echo '{"role": "master"}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 & FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

node() { # node <n> <shard|spread> - tcp 2062n, http 2063n, resp 2064n
	rm -rf "$D/state$1"; mkdir -p "$D/state$1"
	if [ "$2" = shard ]; then mc=239.255.79.72:20619 ex=; else mc=239.255.79.73:20629 ex="replicas = 2"; fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 32
[secrets]
client = uprp-client-secret
cluster = uprp-cluster-secret
[listen]
tcp = 127.0.0.1:2062$1
http = 127.0.0.1:2063$1
resp = 127.0.0.1:2064$1
plaintext = loopback
[cluster]
multicast = $mc
advertise = 127.0.86.$1
mode = $2
$ex
collections = 0
pull_timeout_ms = 200
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = ro:*
upstream_timeout_ms = 1000
upstream_negative_ms = 5000
C
	chmod 600 "$D/n$1.conf"
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "member $1 ($2) did not start: $(tail -2 "$D/n$1.log")"; echo "upreadplacetest: $pass passed, $fail failed"; exit 1; }
}
fleet() { # fleet <shard|spread> - three members up and ready
	for p in $P1ID $P2ID $P3ID; do kill $p 2>/dev/null; wait $p 2>/dev/null; done
	node 1 $1; node 2 $1; node 3 $1
	i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']")" != 2 ]; do sleep 0.2; i=$((i+1)); done
	for n in 1 2 3; do i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done; done
	sleep 1
	[ "$(st 1 "s['cluster']['peers_up']") $(st 1 "s['state']") $(st 2 "s['state']") $(st 3 "s['state']")" = "2 ready ready ready" ] ||
		bad "$1: the members - peers_up $(st 1 "s['cluster']['peers_up']"), states $(st 1 "s['state']") $(st 2 "s['state']") $(st 3 "s['state']")"
}
st() { # st <n> <python expr over s (the member's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:2063%s/stats" % sys.argv[1], timeout=5))
print(eval(sys.argv[2]))
PY
}
col0() { st "$1" "[c for c in s['collections'] if c['name']=='0'][0]$2"; }
reads() { # reads <n> - "k=value" for the 30 keys through member n, the wrong ones only
	python3 - "$1" <<'PY' 2>&1
import socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 20620 + int(sys.argv[1])), 10))
bad = []
for i in range(1, 31):
    r = pcnative.call(f, "get", col="0", key="ro:k%d" % i)
    e = r.get("error")
    v = "ERR " + str(e) if e else (r["result"].get("value") if r["result"].get("found") else None)
    if v != "v%d" % i:
        bad.append("ro:k%d=%s" % (i, v))
print(" ".join(bad))
PY
}
sum3() { echo $(( $(col0 1 "$1") + $(col0 2 "$1") + $(col0 3 "$1") )); }
cl3() { echo "$(st 1 "s['cluster']['$1']") $(st 2 "s['cluster']['$1']") $(st 3 "s['cluster']['$1']")"; }

# ---- shard ---------------------------------------------------------------------------
fleet shard
r=$(reads 1)
[ -z "$r" ] && ok "1. shard: 30 keys through member 1, every value" ||
	bad "1. shard: through member 1, wrong: $(echo "$r" | cut -c1-200) ($(echo "$r" | wc -w) of 30)"
f=$(sum3 "['upstream']['fetches']"); s=$(sum3 "['upstream']['shadow_entries']")
set -- $(cl3 pull_fetching); pf2=$2 pf3=$3; set -- $(cl3 pull_fetched); pd2=$2 pd3=$3
f2=$(col0 2 "['upstream']['fetches']"); f3=$(col0 3 "['upstream']['fetches']")
s1=$(col0 1 "['upstream']['shadow_entries']"); s2=$(col0 2 "['upstream']['shadow_entries']"); s3=$(col0 3 "['upstream']['shadow_entries']")
[ "$f" = 30 ] && [ "$s" = 30 ] && [ "$s1" -lt 30 ] && [ "$s2" -lt 30 ] && [ "$s3" -lt 30 ] &&
	[ $((pf2 + pf3)) -gt 0 ] && [ $((pf2 + pf3)) -eq $((pd2 + pd3)) ] && [ $((pf2 + pf3)) -eq $((f2 + f3)) ] &&
	ok "2. shard: 30 fetches in all, copies $s1 / $s2 / $s3 on the owners, the owners fetched for $((pf2 + pf3)) pulls" ||
	bad "2. shard: fetches $f (30), copies $s1 / $s2 / $s3 (30, none all), pull_fetching $pf2/$pf3, pull_fetched $pd2/$pd3, owners' fetches $f2/$f3"
r=$(reads 2)
[ -z "$r" ] && [ "$(sum3 "['upstream']['fetches']")" = 30 ] && ok "3. shard: the same keys through member 2, found, no second fetch" ||
	bad "3. shard: through member 2 wrong: $(echo "$r" | cut -c1-160), fetches $(sum3 "['upstream']['fetches']") (30)"

# ---- spread --------------------------------------------------------------------------
fleet spread
r=$(reads 1)
[ -z "$r" ] && ok "4. spread: 30 keys through member 1, every value" ||
	bad "4. spread: through member 1, wrong: $(echo "$r" | cut -c1-200) ($(echo "$r" | wc -w) of 30)"
f=$(sum3 "['upstream']['fetches']"); s=$(sum3 "['upstream']['shadow_entries']")
s1=$(col0 1 "['upstream']['shadow_entries']"); set -- $(cl3 pull_fetching); pf=$(( $2 + $3 ))
[ "$f" = 30 ] && [ "$s" = 30 ] && [ "$s1" -lt 30 ] && [ "$pf" -gt 0 ] &&
	ok "5. spread: 30 fetches in all, 30 copies (member 1 $s1), members 2 and 3 fetched for $pf pulls" ||
	bad "5. spread: fetches $f (30), copies $s (30; member 1 $s1, under 30), pull_fetching on 2 and 3: $pf"

for n in 1 2 3; do grep -h 'an fsync has been stuck' "$D/n$n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"; grep "CRIT" "$D/n$n.log" | grep -qv 'an fsync has been stuck' && bad "member $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v 'an fsync has been stuck' | head -n 2 | tr '\n' ' ' | cut -c1-200)"; done
echo "upreadplacetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
