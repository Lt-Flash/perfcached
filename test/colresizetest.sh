#!/bin/sh
# colresizetest.sh - an online resize reaches the fleet AS A RESIZE, not a drop: an EAGER pair with the
# collection declared in both configs (store mode does not reproduce the bug).
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

[ -x "$CLI" ] || { echo "colresizetest: $CLI not built - SKIPPED, and a skip is not a pass"; exit 0; }

D=$(mktemp -d /var/tmp/pccr.XXXXXX)
P1=; P2=
trap 'for p in $P1 $P2; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

if ss -ltn 2>/dev/null | grep -qE ":1798[12][[:space:]]"; then
	echo "colresizetest: port 17981/17982 already bound:" >&2
	ss -ltnp 2>/dev/null | grep -E ":1798[12][[:space:]]" >&2
	exit 1
fi

for n in 1 2; do
	mkdir -p "$D/s$n"
	cat > "$D/n$n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s$n
[memory]
arena_mb = 256
[secrets]
client = cr-client
cluster = cr-cluster
enable = cr-enable
[listen]
tcp = 127.0.0.1:1798$n
plaintext = loopback
[cluster]
multicast = 239.255.77.71:17171
advertise = 127.0.2.8$n
mode = eager
collections = th
[collection th]
buckets_log2 = 16
CONF
done

: > "$D/n1.log"
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
: > "$D/n2.log"
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null &&
		grep -q "perfcached ready" "$D/n2.log" 2>/dev/null && break
	sleep 0.1; i=$((i + 1))
done
[ $i -lt 200 ] || { echo "  fleet did not start"; exit 1; }
# a joiner is CONNECTING until its links answer: up to 30 s for both ready and seeing each other
st() { "$CLI" -q -h 127.0.0.1 -p "$1" -j '{"method":"stats"}' 2>/dev/null \
	| python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
i=0
while [ $i -lt 300 ] && { [ "$(st 17981)" != "ready 1" ] || [ "$(st 17982)" != "ready 1" ]; }; do
	sleep 0.1; i=$((i + 1))
done

# buckets of collection $2 on port $1, or "gone"
bkt() { "$CLI" -q -h 127.0.0.1 -p "$1" collections 2>/dev/null | python3 -c "
import json, sys
want = sys.argv[1]
try:
    cols = json.load(sys.stdin)['collections']
except Exception:
    print('error'); sys.exit(0)
for c in cols:
    if c['name'] == want:
        print(c['buckets']); break
else:
    print('gone')" "$2"; }

[ "$(bkt 17981 th)" = 65536 ] && [ "$(bkt 17982 th)" = 65536 ] \
	&& ok "both nodes start with th at 2^16" \
	|| { bad "premise: n1=$(bkt 17981 th) n2=$(bkt 17982 th), want 65536 each"; \
	     echo "colresizetest: $pass passed, $fail failed"; exit 1; }

# a handful of writes first, or the assertion below has nothing to preserve and cannot fail
for K in a b c d e f g h; do
	"$CLI" -q -h 127.0.0.1 -p 17981 set th pre-$K v 300 >/dev/null 2>&1
	"$CLI" -q -h 127.0.0.1 -p 17981 get th pre-$K >/dev/null 2>&1
done

# a resize builds a NEW table, whose counters must carry the collection's lifetime figures: record
# what it stored BEFORE the resize
ST_BEFORE=$("$CLI" -q -h 127.0.0.1 -p 17981 stats th 2>/dev/null \
	| python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: print(0); raise SystemExit
c=[x for x in d.get("collections",[]) if x.get("name")=="th"]
print(c[0].get("hits",0) if c else 0)')

R=$("$CLI" -q -h 127.0.0.1 -p 17981 -E cr-enable resize th 12 2>&1 | head -1)
case "$R" in
  *'"resizing"'*) ok "the resize was accepted on node 1" ;;
  *) bad "resize refused: $R" ;;
esac

i=0
while [ $i -lt 60 ]; do
	[ "$(bkt 17982 th)" = 4096 ] && break
	sleep 0.5; i=$((i + 1))
done

N2=$(bkt 17982 th)
[ "$N2" != gone ] \
	&& ok "the peer still HAS the collection - the resize was not announced as a drop" \
	|| bad "the peer DELETED th: a resize was broadcast as a drop (this is the bug)"
[ "$N2" = 4096 ] \
	&& ok "and the peer is at the new size (4096, after $(echo "$i" | awk '{printf "%.1f", $1/2}')s)" \
	|| bad "peer buckets: $N2, want 4096"
# the originator needs its own wait: the peer adopts the announced size at once, while the originator
# moves its records incrementally, so the peer finishing first is legitimate
j=0
while [ $j -lt 60 ]; do
	[ "$(bkt 17981 th)" = 4096 ] && break
	sleep 0.5; j=$((j + 1))
done
[ "$(bkt 17981 th)" = 4096 ] \
	&& ok "the originating node resized too (after $(echo "$j" | awk '{printf "%.1f", $1/2}')s)" \
	|| bad "originator buckets: $(bkt 17981 th), want 4096 - after 30s, so this is not slowness"

ST_AFTER=$("$CLI" -q -h 127.0.0.1 -p 17981 stats th 2>/dev/null \
	| python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: print(0); raise SystemExit
c=[x for x in d.get("collections",[]) if x.get("name")=="th"]
print(c[0].get("hits",0) if c else 0)')
[ "${ST_BEFORE:-0}" -gt 0 ] && [ "${ST_AFTER:-0}" -ge "${ST_BEFORE:-0}" ] \
	&& ok "the collection's lifetime counters survived the resize (hits $ST_BEFORE -> $ST_AFTER)" \
	|| bad "counters reset across the resize: hits $ST_BEFORE -> $ST_AFTER (S184)"

"$CLI" -q -h 127.0.0.1 -p 17982 set th k1 v1 60 >/dev/null 2>&1
case "$("$CLI" -q -h 127.0.0.1 -p 17982 get th k1 2>&1)" in
  *'"found"'*true*) ok "the peer's collection still takes writes and serves reads" ;;
  *) bad "peer th is not usable after the resize" ;;
esac

echo "colresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
