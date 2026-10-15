#!/bin/sh
# standalonetest.sh - S158: a standalone daemon (no [cluster] section)
# ACCEPTS the fleet-shaped collection keys, runs them as store, and says
# so once at WARNING - so one config file serves both shapes.  Also: the
# cluster secret is optional standalone (nothing derives from it there)
# and still required, and still distinct, where a [cluster] section is.
# Optional means every reader copes with its absence: 0.3.8-rc1 through
# 0.4.0-rc3 read the missing secret in three places and a standalone
# daemon SEGV'd on -E, on a resp password, and at startup whenever a
# door was encryption-required.  And an underived cluster key is all
# zeros, so the door must refuse that principal outright rather than
# hand-shake with a key anyone can present.
# Usage: test/standalonetest.sh [./perfcached] [./noisetest]
set -u
BIN=${1:-./perfcached}
NT=${2:-./noisetest}
D=$(mktemp -d /var/tmp/pcstandalone.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cfg() { # cfg <name> <secrets-extra> <cluster-block> <collection-extra>
	cat > "$D/$1.conf" <<CONF
[daemon]
workers = 1
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = standalone-client-secret
$2
[listen]
tcp = 127.0.0.1:17441
plaintext = loopback
$3
[collection a]
buckets_log2 = 12
$4
CONF
	chmod 640 "$D/$1.conf"
}
run() { "$BIN" -f "$D/$1.conf" -C > "$D/$1.out" 2>&1; echo $?; }
CL="[cluster]
multicast = 239.255.77.49:17149
advertise = 127.0.12.1
mode = shard
collections = a"
CS="cluster = standalone-cluster-secret"
mkdir -p "$D/state"

cfg a "$CS" "" "mode = shard
pull = 1"
[ "$(run a)" = 0 ] && ok "standalone: mode = shard + pull = 1 validates" || bad "standalone: mode = shard + pull = 1 refused: $(tail -1 $D/a.out)"
grep -q "collection 'a': mode = shard ignored - no \[cluster\] section" "$D/a.out" && ok "  ...and says mode = shard is ignored" || bad "  ...no 'mode = shard ignored' warning"
grep -q "collection 'a': pull = 1 ignored" "$D/a.out" && ok "  ...and says pull = 1 is ignored" || bad "  ...no 'pull = 1 ignored' warning"

cfg b "$CS" "$CL" "mode = shard
pull = 1"
[ "$(run b)" = 0 ] && ok "the same stanza under [cluster] validates" || bad "the same stanza under [cluster] refused: $(tail -1 $D/b.out)"
grep -q "ignored" "$D/b.out" && bad "  ...but warns 'ignored' with a cluster present" || ok "  ...with no 'ignored' warning: shard is honoured"

cfg c "$CS" "" "mode = spread"
[ "$(run c)" = 0 ] && grep -q "mode = spread ignored" "$D/c.out" && ok "standalone: mode = spread is inert like the rest" || bad "standalone: mode = spread not accepted-and-warned: $(tail -1 $D/c.out)"
cfg d "$CS" "$CL" "mode = spread"
[ "$(run d)" != 0 ] && grep -q "not a per-collection setting" "$D/d.out" && ok "with a cluster, per-collection spread is still refused (K is cluster state)" || bad "per-collection spread under [cluster] was not refused"

cfg e "$CS" "" "mode = store"
[ "$(run e)" = 0 ] && ! grep -q "ignored" "$D/e.out" && ok "negative control: standalone mode = store warns nothing" || bad "standalone mode = store: $(grep -c ignored $D/e.out) 'ignored' line(s)"

sed 's/^workers = 1/workers = 1\nmode = shard/' "$D/e.conf" > "$D/f.conf"; chmod 640 "$D/f.conf"
[ "$(run f)" != 0 ] && grep -q "unknown key 'mode'" "$D/f.out" && ok "[daemon] mode is still an unknown key" || bad "[daemon] mode was not refused"

cfg g "" "" ""
[ "$(run g)" = 0 ] && ok "standalone without a cluster secret validates" || bad "standalone without a cluster secret refused: $(tail -1 $D/g.out)"
cfg h "" "$CL" ""
[ "$(run h)" != 0 ] && grep -q "no cluster secret" "$D/h.out" && ok "[cluster] without a cluster secret is refused" || bad "[cluster] without a cluster secret was not refused"
cfg i "cluster = standalone-client-secret" "" ""
[ "$(run i)" != 0 ] && grep -q "EQUALS client secret" "$D/i.out" && ok "a cluster secret equal to the client secret is refused even standalone" || bad "equal secrets accepted standalone"

"$BIN" -f "$D/g.conf" -E > "$D/g.dump" 2>&1; rc=$?
[ $rc = 0 ] && grep -q "^cluster = (not set)" "$D/g.dump" && ok "standalone: -E dumps an absent cluster secret as not set" || bad "standalone: -E without a cluster secret: exit $rc, $(grep -c '^cluster' "$D/g.dump") cluster line(s)"
cfg j "resp = standalone-resp-password" "resp = 127.0.0.1:17438" ""
[ "$(run j)" = 0 ] && ok "standalone: a resp password without a cluster secret validates" || bad "standalone: a resp password without a cluster secret: exit $(run j), $(tail -1 $D/j.out)"
cfg k "resp = standalone-cluster-secret
$CS" "resp = 127.0.0.1:17438" ""
[ "$(run k)" != 0 ] && grep -q "EQUALS the cluster" "$D/k.out" && ok "a resp password equal to a present cluster secret is still refused" || bad "a resp password equal to the cluster secret was not refused"

# live: an encryption-required door on a standalone daemon
live() { # live <name> <secrets-extra>: start it, 0 once ready
	cat > "$D/$1.conf" <<CONF
[daemon]
workers = 1
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = standalone-client-secret
$2
[listen]
tcp = 127.0.0.1:17437
plaintext = never
[collection a]
buckets_log2 = 12
CONF
	chmod 640 "$D/$1.conf"
	"$BIN" -f "$D/$1.conf" -D > "$D/$1.log" 2>&1 & P1=$!
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/$1.log" && return 0
		kill -0 "$P1" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stop() { kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=; }
dial() { "$NT" dial 127.0.0.1 17437 "$1" "$2"; }
CK=$("$NT" psk client standalone-client-secret)
WK=$("$NT" psk client not-the-client-secret)
KK=$("$NT" psk cluster standalone-cluster-secret)

if live m ""; then
	ok "standalone: an encryption-required door without a cluster secret starts"
	[ "$(dial client "$CK")" = established ] && ok "  ...a client with the client secret completes the handshake" || bad "  ...the client secret's handshake did not complete"
	[ "$(dial client "$WK")" = refused ] && ok "  ...negative control: a wrong client key is refused" || bad "  ...a wrong client key was not refused"
	[ "$(dial cluster zero)" = refused ] && ok "  ...the cluster principal is refused with the all-zero key" || bad "  ...the cluster principal hand-shook with the ALL-ZERO key"
	[ "$(dial cluster "$KK")" = refused ] && ok "  ...and with any cluster key: there is no cluster secret to match" || bad "  ...the cluster principal hand-shook with no cluster secret configured"
	stop
else
	stop
	bad "standalone: an encryption-required door without a cluster secret did not start: $(tail -1 "$D/m.log")"
fi
if live n "$CS"; then
	[ "$(dial cluster "$KK")" = established ] && ok "positive control: with a cluster secret the cluster principal hand-shakes" || bad "positive control: the cluster secret's handshake did not complete"
	[ "$(dial cluster zero)" = refused ] && ok "  ...and the all-zero key is still refused" || bad "  ...the all-zero key hand-shook with a cluster secret set"
	stop
else
	stop
	bad "standalone with a cluster secret did not start: $(tail -1 "$D/n.log")"
fi

echo "standalonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
