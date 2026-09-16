#!/bin/sh
# standalonetest.sh - S158: a standalone daemon (no [cluster] section)
# ACCEPTS the fleet-shaped collection keys, runs them as store, and says
# so once at WARNING - so one config file serves both shapes.  Also: the
# cluster secret is optional standalone (nothing derives from it there)
# and still required, and still distinct, where a [cluster] section is.
# Usage: test/standalonetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcstandalone.XXXXXX); trap 'rm -rf "$D"' EXIT INT TERM
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

echo "standalonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
