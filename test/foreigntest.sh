#!/bin/sh
# foreigntest.sh - S36 step d: a SECOND cluster on the same seeds is an
# alarm, never a merge.
#
# In unicast mode every node beats at every seed, so two fleets that
# share a seed list hear each other all the time.  S302 already refuses
# the other cluster's frames; this asserts the operator is TOLD: an
# ERROR the first time a foreign id is heard, /stats
# cluster.foreign_clusters naming it, and the /metrics gauge.
#   two fleets pinned to different ids (alpha: nodes 1, 2; beta: 3, 4),
#   every node seeded with node 1 and node 3:
#   1  each fleet stays two members - no merge
#   2  each side lists the other's id in foreign_clusters, from a member
#      of the other fleet
#   3  the ERROR is logged once per foreign id per node, not per frame
#   4  /metrics perfcached_cluster_foreign_clusters is 1
# Usage: test/foreigntest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfor.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18140

conf() { # conf <n> <pin>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 16
[secrets]
client = fo-client-secret
cluster = fo-cluster-secret
[listen]
tcp = 127.0.0.1:$((18150 + $1))
http = 127.0.0.1:$((18160 + $1))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.35.1, 127.0.35.3
advertise = 127.0.35.$1
cluster_id = $2
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"; mkdir -p "$D/s$1"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"; }
st() {
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18160 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
up() { st "$1" 'r["cluster"]["peers_up"]'; }
wait_for() {
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

conf 1 alpha; conf 2 alpha; conf 3 beta; conf 4 beta
for n in 1 2 3 4; do start $n; done
wait_for 40 '[ "$(up 1)$(up 2)$(up 3)$(up 4)" = 1111 ]' \
	&& ok "two fleets of two - alpha and beta did not merge" \
	|| bad "peers $(up 1)/$(up 2)/$(up 3)/$(up 4) - want 1 each"
sleep 4
[ "$(up 1)$(up 3)" = 11 ] && ok "and stay two after hearing each other" || bad "a fleet changed size ($(up 1)/$(up 3))"
IA=$(st 1 'r["cluster"]["cluster_id"]'); IB=$(st 3 'r["cluster"]["cluster_id"]')
FA=$(st 1 '",".join(f["id"] for f in r["cluster"]["foreign_clusters"])')
FB=$(st 3 '",".join(f["id"] for f in r["cluster"]["foreign_clusters"])')
[ "$FA" = "$IB" ] && [ "$FB" = "$IA" ] && ok "each side lists the other's id in foreign_clusters" \
	|| bad "foreign_clusters: node 1 '$FA' (want $IB), node 3 '$FB' (want $IA)"
FR=$(st 1 'r["cluster"]["foreign_clusters"][0]["from"]')
case "$FR" in 127.0.35.3:*|127.0.35.4:*) ok "node 1 heard beta from a beta member ($FR)" ;;
	*) bad "node 1's foreign entry is from '$FR'" ;; esac
sleep 3
E1=$(grep -c "a SECOND cluster is on this network" "$D/n1.log"); E3=$(grep -c "a SECOND cluster is on this network" "$D/n3.log")
[ "$E1" = 1 ] && [ "$E3" = 1 ] && ok "the ERROR is said once per node per foreign id (node 1: $E1, node 3: $E3)" \
	|| bad "ERROR lines: node 1 $E1, node 3 $E3 - want 1 each"
M=$(curl -s http://127.0.0.1:18161/metrics | sed -n 's/^perfcached_cluster_foreign_clusters \([0-9]*\)$/\1/p')
[ "$M" = 1 ] && ok "/metrics perfcached_cluster_foreign_clusters 1" || bad "the gauge reads '$M'"

for f in "$D"/*.pid; do kill "$(cat "$f")" 2>/dev/null; done
echo "foreigntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
