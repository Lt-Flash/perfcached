#!/bin/sh
# phpfleettest.sh - the PHP extension against a three-node fleet (test/phpfleettest.php): failover past a
# killed node, a forked child's own channel, ownerOf() agreeing with the daemons, a list of seeds, and
# the Symfony adapter's clear() on a shard collection.
set -u
BIN=${1:-./perfcached}
PHP=${2:-php8.3}
EXT=${3:-lang/php/modules/perfcached.so}
VND=${4:-.phpvendor/vendor/autoload.php}
case $EXT in /*) ;; *) EXT=$(pwd)/$EXT ;; esac
case $VND in /*) ;; *) VND=$(pwd)/$VND ;; esac
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/.." && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcpfl.XXXXXX)
P1= P2= P3=
down() { for p in $P1 $P2 $P3; do kill -9 $p 2>/dev/null; done; wait 2>/dev/null; P1= P2= P3=; }
trap 'down; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
SEC=php-fleet-secret
for p in 19591 19592 19593 19594 19595 19596 19610; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "phpfleettest: port $p already bound" >&2; exit 1; }
done
fleet() { # fleet <mode>
	down
	rm -rf "$D"/n* "$D"/s*
	for i in 1 2 3; do
		mkdir -p "$D/s$i"
		cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = php-fleet-cluster
[listen]
tcp = 127.0.163.$i:19591
http = 127.0.163.$i:1959$((i + 3))
plaintext = never
[cluster]
discovery = unicast
port = 19610
seeds = 127.0.163.1
advertise = 127.0.163.$i
mode = $1
collections = c
[collection c]
buckets_log2 = 10
C
		chmod 600 "$D/n$i.conf"
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
		eval "P$i=\$!"
		sleep 0.3
	done
	i=0; while [ $i -lt 300 ]; do
		[ "$(st 1 peers)$(st 2 peers)$(st 3 peers)" = 222 ] && [ "$(st 1 state)$(st 2 state)$(st 3 state)" = readyreadyready ] && return 0
		sleep 0.2; i=$((i+1))
	done
	echo "phpfleettest: the $1 fleet did not form: $(st 1 peers)/$(st 2 peers)/$(st 3 peers) $(tail -2 "$D/n1.log" | tr '\n' ' ' | cut -c1-200)"
	exit 1
}
st() { # st <i> <peers|state|entries>
	curl -s -m 3 "http://127.0.163.$1:1959$(($1 + 3))/stats" 2>/dev/null | python3 -c '
import json, sys
r = json.load(sys.stdin); w = sys.argv[1]
if w == "peers": print(r["cluster"]["peers_up"])
elif w == "state": print(r["state"])
else: print(sum(c["entries"] for c in r["collections"] if c["name"] == "c"))' "$2" 2>/dev/null || echo "?"
}
PX() { "$PHP" -d extension="$EXT" "$T/phpfleettest.php" "$@"; }

echo "--- eager: failover under load"
fleet eager
PX failover "$SEC" "$P1" "$P2" "$P3" || fail=$((fail+1))
echo "--- eager: fork"
fleet eager
PX fork "$SEC" || fail=$((fail+1))
echo "--- eager: a list of seeds"
fleet eager
PX seeds "$SEC" "$P1" "$P2" "$P3" || fail=$((fail+1))
echo "--- shard: ownerOf"
fleet shard
PX owners "$SEC" "$D/owners" || fail=$((fail+1))
C=0; while read -r node addr want; do
	i=${addr##*.}; got=$(st "$i" entries)
	[ "$got" = "$want" ] && C=$((C+1))
	echo "  ..   node $node ($addr): ownerOf says $want keys, the daemon holds $got"
done < "$D/owners"
[ "$C" = 3 ] && ok "ownerOf() agrees with where every key lives, on all three nodes" \
	|| bad "ownerOf() and the daemons disagree on $((3 - C)) node(s)"
echo "--- shard: the Symfony adapter's clear()"
if [ -f "$VND" ]; then
	fleet shard
	PX clear "$SEC" "$VND" "$R/lang/php/symfony/src/PerfcachedAdapter.php" || fail=$((fail+1))
	echo "--- shard: clear() with every other node down"
	kill -9 $P2 $P3 2>/dev/null; wait $P2 $P3 2>/dev/null; P2= P3=
	i=0; while [ $i -lt 100 ] && [ "$(st 1 peers)" != 0 ]; do sleep 0.2; i=$((i+1)); done
	if [ "$(st 1 peers)" = 0 ]; then
		PX clearalone "$SEC" "$VND" "$R/lang/php/symfony/src/PerfcachedAdapter.php" || fail=$((fail+1))
	else
		bad "node 1 still sees $(st 1 peers) peer(s) up after nodes 2 and 3 were killed"
	fi
else
	bad "no $VND: make phpvendor"
fi
echo "phpfleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
