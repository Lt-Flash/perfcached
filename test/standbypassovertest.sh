#!/bin/sh
# standbypassovertest.sh - S277: a standby that stops answering is passed
# over, and the master keeps publishing maps.
#
# A map change is staged past the standby and broadcast only once it has
# acked; an unacked stage is abandoned after CLSYNC_ACK_MS.  But the next
# stage picked the standby the same way - the lowest live peer id - so a
# standby that was LIVE (its multicast ALIVE still arriving) and could not
# answer was named again, every time, and the master published nothing for
# as long as that lasted.  rc57's GitHub check-asan (restartsharetest,
# shard): the restarted member had been the standby, the master froze on a
# map without it, and a member placing from that map answered 267 of 800
# names wrong for 150 s.
#
# Three shard nodes.  The standby is cut from the master's unicast AND its
# maps (the netcut shim, by peer address); its own multicast still goes
# out, so the master keeps counting it live.  Then:
#   1. within 13 s the master publishes at least two more maps (rc57:
#      none) - the lapse costs CLSYNC_ACK_MS, then one per CLMAP_PUB_MS
#   2. the map names another standby, and sync.timeouts counts the lapse
#   3. the third node - not cut - receives those maps and its map is usable
#   4. healed, publication continues
# Fail-first: rc57 publishes nothing in step 1 and has no sync.timeouts.
# Usage: test/standbypassovertest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
PRELOAD="${SANRT:+$SANRT }$SHIM"
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "standbypassovertest: no shim at $SHIM" >&2; exit 1; }
for p in 18641 18642 18643 18651 18652 18653; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "standbypassovertest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n>
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = spo-client-secret
cluster = spo-cluster-secret
[listen]
tcp = 127.0.0.1:1864$1
http = 127.0.0.1:1865$1
plaintext = loopback
[cluster]
multicast = 239.255.78.11:18645
advertise = 127.0.27.$1
mode = shard
collections = c
[collection c]
buckets_log2 = 10
C
	mkdir -p "$D/s$1"
	chmod 600 "$D/n$1.conf"
}
start() { # start <n>
	LD_PRELOAD="$PRELOAD" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
st() { curl -s "http://127.0.0.1:1865$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
up() { st $1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))'; }

D=$(mktemp -d /var/tmp/pcspo.XXXXXX)
for n in 1 2 3; do conf $n; done
for n in 1 2 3; do start $n || exit 1; done
i=0; while [ $i -lt 120 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
		[ "$(up 1)$(up 2)$(up 3)" = 222 ] && break
	sleep 0.5; i=$((i+1))
done
M=; for n in 1 2 3; do [ "$(st $n 'r["cluster"]["role"]')" = master ] && M=$n; done
[ -n "$M" ] || { echo "standbypassovertest: the fleet did not form"; exit 1; }
# the standby is named in a map; wait for one naming all three and a standby
i=0; while [ $i -lt 150 ] && { [ "$(st $M 'r["cluster"]["map"]["nodes"]')" != 3 ] || \
		[ "$(st $M 'r["cluster"]["map"]["backup"]')" = 0 ]; }; do sleep 0.2; i=$((i+1)); done
BK=$(st $M 'r["cluster"]["map"]["backup"]')
B=; for n in 1 2 3; do [ "$(st $n 'r["cluster"]["node"]')" = "$BK" ] && B=$n; done
[ -n "$B" ] && [ "$B" != "$M" ] || { echo "standbypassovertest: no standby named (map backup ${BK:-?})"; exit 1; }
O=; for n in 1 2 3; do [ $n != $M ] && [ $n != $B ] && O=$n; done
pub() { st $M 'r["cluster"]["map"]["published"]'; }
dump() {
	for n in 1 2 3; do
		echo "      node $n: $(st $n '"%s %s, map %s, peers %s" % (r["state"], r["cluster"].get("role"), json.dumps(r["cluster"].get("map")), [(p.get("node"), p.get("up"), p.get("state")) for p in r["cluster"]["peers"]])')"
		grep -hiE "standby|master|term|map" "$D/n$n.log" | tail -4 | sed 's/^/        /'
	done
}
echo "  master node $M, standby node $B (id $BK), the other node $O"

P0=$(pub); R0=$(st $O 'r["cluster"]["map"]["received"]')
# the standby stops hearing the master - unicast and maps; its own
# multicast ALIVE still reaches the master, so it stays live there
echo "cut 127.0.27.$M" > "$D/cut$B"
sleep 13
P1=$(pub); R1=$(st $O 'r["cluster"]["map"]["received"]')
BK1=$(st $M 'r["cluster"]["map"]["backup"]')
TO=$(st $M 'r["cluster"]["sync"].get("timeouts", "absent")')
[ "$(st $M 'sum(1 for p in r["cluster"]["peers"] if p.get("up") and p.get("node") == '"$BK"')')" = 1 ] \
	|| echo "  (note: the master no longer counts the cut standby live - the shim cut its multicast too)"
[ "${P1:-0}" -ge $((${P0:-0} + 2)) ] 2>/dev/null \
	&& ok "1: the master kept publishing with its standby silent ($P0 -> $P1 in 13 s)" \
	|| bad "1: the master stopped publishing behind a silent standby ($P0 -> ${P1:-?} in 13 s)"
[ -n "$BK1" ] && [ "$BK1" != "$BK" ] && [ "$TO" != absent ] && [ "$TO" -ge 1 ] 2>/dev/null \
	&& ok "2: the silent standby was passed over (standby $BK -> $BK1, sync.timeouts $TO)" \
	|| bad "2: standby ${BK1:-?} (was $BK), sync.timeouts $TO - want another standby and >= 1"
# usable is retaken every tick, and briefly false while a map and the
# liveness it describes cross; allow it 5 s
i=0; while [ $i -lt 25 ]; do
	U=$(st $O 'r["cluster"]["map"].get("usable")'); [ "$U" = True ] && break
	sleep 0.2; i=$((i+1))
done
[ "${R1:-0}" -gt "${R0:-0}" ] 2>/dev/null && [ "$U" = True ] \
	&& ok "3: node $O received the new maps ($R0 -> $R1) and its map is usable" \
	|| { bad "3: node $O received $R0 -> ${R1:-?}, usable $U"; dump; }
grep -q "passing it over" "$D/n$M.log" \
	&& ok "3b: the master says so: $(grep -o 'the standby (node [0-9]*) has not acknowledged[^-]*' "$D/n$M.log" | head -1)" \
	|| bad "3b: no warning naming the passed-over standby"

: > "$D/cut$B"
sleep 8
P2=$(pub)
[ "${P2:-0}" -gt "${P1:-0}" ] 2>/dev/null \
	&& ok "4: healed, publication continues ($P1 -> $P2)" \
	|| bad "4: healed, published $P1 -> ${P2:-?}"
echo "standbypassovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
