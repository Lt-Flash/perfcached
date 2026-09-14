#!/bin/sh
# colresizetest.sh — an online resize must reach the fleet AS A RESIZE.
#
# THE BUG THIS EXISTS FOR.  pc_cluster_col_announce() took an int named
# `drop` and folded it: `op = drop ? CLCOL_OP_DROP : CLCOL_OP_SET`.
# Every caller already passed a CLCOL_OP_* value, so create (0) and drop
# (1) came out right by coincidence - and RESIZE (3), being merely
# truthy, was broadcast to the fleet AS A DROP.  Every peer deleted the
# collection, and the originator lost it too as the drops converged
# back.  The call returned {"resizing":true} and the daemon logged a
# resize; nothing said anything had been dropped.
#
# Found on the live fleet 245-247 on 2026-09-15 against an EMPTY
# collection.  With records in it that was fleet-wide data loss.
#
# WHY A SUITE OF ITS OWN, and not coltest's fleet section: coltest runs
# its pair in `mode = store`, and store mode does NOT reproduce this -
# verified, the buggy binary passes there.  It needs an EAGER pair with
# the collection DECLARED in both configs, which is the shape the live
# fleet has.  A test that cannot fail on the unfixed build proves
# nothing, so this one was checked against it first.
#
# usage: test/colresizetest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

[ -x "$CLI" ] || { echo "colresizetest: $CLI not built - SKIPPED, and a skip is not a pass"; exit 0; }

D=$(mktemp -d /var/tmp/pccr.XXXXXX)
P1=; P2=
trap 'for p in $P1 $P2; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM

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

"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null &&
		grep -q "perfcached ready" "$D/n2.log" 2>/dev/null && break
	sleep 0.1; i=$((i + 1))
done
[ $i -lt 200 ] || { echo "  fleet did not start"; exit 1; }
sleep 5

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
[ "$(bkt 17981 th)" = 4096 ] \
	&& ok "the originating node resized too" \
	|| bad "originator buckets: $(bkt 17981 th), want 4096"

"$CLI" -q -h 127.0.0.1 -p 17982 set th k1 v1 60 >/dev/null 2>&1
case "$("$CLI" -q -h 127.0.0.1 -p 17982 get th k1 2>&1)" in
  *'"found"'*true*) ok "the peer's collection still takes writes and serves reads" ;;
  *) bad "peer th is not usable after the resize" ;;
esac

echo "colresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
