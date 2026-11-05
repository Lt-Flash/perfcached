#!/bin/sh
# autoscalefleettest.sh - S436: a collection's autoscale mode is the fleet's.
#
# Seen on 245-247 (rc11, 2026-10-23): `autoscale thbench floor` on the master
# changed the master only; the other two kept auto and shrank the table to
# 34,892 buckets while the master stayed at 4,194,304 - one collection, a
# 120x different table, and nothing said so.  Three nodes (shard), c at 2^12,
# autoscale from the config (auto), shrink_cooloff_s = 2:
#   1. node 3 stands in for a build before 0.7.2 (PERFCACHED_TEST_NO_AUTOSCALE):
#      `autoscale c floor` through node 1 is refused, naming it;
#   2. node 3 stopped: the verb through node 1 is accepted, and node 2 takes
#      floor at node 1's generation;
#   3. node 3 back as the older build: the cluster.autoscale alert names it;
#   4. node 3 back as this build: it takes the fleet's floor (the resync), and
#      the alert clears;
#   5. `resize c 14` through node 2: every table reaches 16,384 buckets and
#      STAYS there past the cool-off - under auto, 0 keys shrink it to 4,096;
#   6. node 2 restarted: floor and its generation come back from its
#      collections file, and the resync's re-sends log nothing again;
#   7. node 3 deaf to the fleet's modes (PERFCACHED_TEST_AUTOSCALE_DEAF):
#      `autoscale c warn` through node 1 reaches node 2 and not node 3, and
#      the alert names node 3 as differing - its stats too.
# FAIL-FIRST: the build before S436 accepts step 1, never moves node 2 off
# auto (step 2), raises no alert (3, 7) and shrinks nodes 2 and 3 (5).
# Usage: test/autoscalefleettest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcasf.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19551 19552 19553 19561 19562 19563; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "autoscalefleettest: port $p busy" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=10          # cluster.autoscale's 30 s hold -> 3 s

for n in 1 2 3; do
	mkdir -p "$D/s$n/wal"
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$n
allow_create = yes
shrink_cooloff_s = 2
[memory]
arena_mb = 64
[secrets]
client = asf-client-secret
cluster = asf-cluster-secret
enable = asf-enable-secret
[listen]
tcp = 127.0.0.1:1955$n
http = 127.0.0.1:1956$n
plaintext = loopback
[cluster]
multicast = 239.255.66.36:19559
advertise = 127.0.20.$n
pull_timeout_ms = 400
mode = shard
collections = c
[collection c]
buckets_log2 = 12
[wal]
dir = $D/s$n/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$n.conf"
done

start() { # start <n> [VAR=value]: each start logs to its own file, n<n>.log.<k>
	k=$(ls "$D"/n$1.log.* 2>/dev/null | wc -l); L="$D/n$1.log.$k"
	if [ -n "${2:-}" ]; then env "$2" "$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	else "$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	fi
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$L" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$L" | tr '\n' ' ')"
	return 1
}
stop() { # stop <n>: a clean stop (it says goodbye), waited for
	P=$(cat "$D/n$1.pid"); kill -TERM "$P" 2>/dev/null
	i=0; while kill -0 "$P" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
	kill -9 "$P" 2>/dev/null; rm -f "$D/n$1.pid"
}
st() { curl -s "http://127.0.0.1:1956$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
up() { st $1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))'; }
mode() { st $1 'c["autoscale"]'; }
gen() { st $1 'c["autoscale_gen"]'; }
bk() { st $1 'c["buckets"]'; }
busy() { st $1 '1 if c.get("resizing_to") else 0'; }
# the cluster.autoscale alert's text on node <n>, or None
alert() { st $1 'next((a.get("text") for a in (r.get("alerts") or {}).get("active") or [] if a.get("key") == "cluster.autoscale"), None)'; }
# what node <n> says of the member at <addr>: autoscale_differs (GET /members)
differs() { curl -s "http://127.0.0.1:1956$1/members" | python3 -c '
import json, sys
r = json.load(sys.stdin)
print(next((m.get("autoscale_differs") for m in r.get("members") or [] if m.get("addr") == sys.argv[1]), "absent"))' "$2" 2>/dev/null; }
ready() { # ready <want>: every node in <want> (e.g. "1 2 3") ready and seeing the others
	want=$(( $(echo $1 | wc -w) - 1 ))
	i=0; while [ $i -lt 120 ]; do
		good=1
		for n in $1; do
			[ "$(st $n 'r["state"]')" = ready ] && [ "$(up $n)" = $want ] || { good=0; break; }
		done
		[ $good = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
priv() { # priv <n> <method> <params json>: enable, then the call - prints its reply
	python3 - "1955$1" "$2" "$3" <<'PY'
import json, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30))
for i, (m, p) in enumerate([("enable", {"secret": "asf-enable-secret"}), (sys.argv[2], json.loads(sys.argv[3]))]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)
PY
}
until_eq() { # until_eq <seconds> <want> <command...>: polls, prints the last value
	t=$1; w=$2; shift 2; i=0; v=
	while [ $i -lt $((t * 5)) ]; do
		v=$("$@"); [ "$v" = "$w" ] && break
		sleep 0.2; i=$((i+1))
	done
	echo "$v"
}
fleetlines() { grep -h "set by the fleet" "$D"/n$1.log.* 2>/dev/null | wc -l; }
# alert_has <n> <pattern>: x when node <n>'s cluster.autoscale text matches, else the text
alert_has() { v=$(alert $1); case "$v" in *$2*) echo x;; *) echo "$v";; esac; }

echo "--- 1. a member on an older build: the verb is refused"
start 1 && start 2 && start 3 PERFCACHED_TEST_NO_AUTOSCALE=1 || { bad "a node did not start"; exit 1; }
ready "1 2 3" || { bad "the fleet did not form"; exit 1; }
R=$(priv 1 autoscale '{"col": "c", "mode": "floor"}')
case "$R" in *older*) ok "1. autoscale c floor refused while node 3 predates it: $(echo "$R" | cut -c1-150)";;
	*) bad "1. autoscale c floor accepted with an older member in the fleet: $R";; esac
[ "$(mode 1)/$(mode 2)/$(mode 3)" = auto/auto/auto ] && ok "1. and nothing changed: auto on all three" \
	|| bad "1. modes after the refusal: $(mode 1)/$(mode 2)/$(mode 3) (want auto/auto/auto)"

echo "--- 2. the older member gone: the mode reaches the fleet"
stop 3
ready "1 2" || bad "2. nodes 1 and 2 did not settle without node 3"
# a member that said goodbye may read live a moment longer: the verb is
# refused until it does not (that refusal is step 1's, not this step's)
i=0; while [ $i -lt 20 ]; do
	R=$(priv 1 autoscale '{"col": "c", "mode": "floor"}')
	case "$R" in *older*) sleep 0.5; i=$((i+1));; *) break;; esac
done
case "$R" in *'"autoscale":"floor"'*) ok "2. autoscale c floor accepted through node 1";;
	*) bad "2. autoscale c floor through node 1: $R";; esac
G1=$(gen 1)
M2=$(until_eq 5 floor mode 2)
[ "$M2" = floor ] && [ "$(gen 2)" = "$G1" ] && [ "$G1" != 0 ] \
	&& ok "2. node 2 took floor at node 1's generation ($G1)" \
	|| bad "2. node 2: mode $M2, generation $(gen 2) (node 1: floor at $G1)"
[ "$(st 2 'c["autoscale_set"]')" = True ] && ok "2. node 2 says the mode was set at runtime" \
	|| bad "2. node 2 autoscale_set: $(st 2 'c["autoscale_set"]')"

echo "--- 3. the older member back: named"
start 3 PERFCACHED_TEST_NO_AUTOSCALE=1 || bad "3. node 3 did not start"
ready "1 2 3" || bad "3. the fleet did not re-form"
A=$(until_eq 15 x alert_has 1 "127.0.20.3")
[ "$A" = x ] && ok "3. node 1 raised cluster.autoscale: $(alert 1 | cut -c1-150)" \
	|| bad "3. no cluster.autoscale on node 1 naming node 3 (127.0.20.3): $A"
[ "$(mode 3)" = auto ] && ok "3. node 3 (the older build) kept auto - which is what the alert is for" \
	|| bad "3. node 3: $(mode 3)"

echo "--- 4. node 3 on this build: it takes the fleet's"
stop 3
start 3 || bad "4. node 3 did not start"
ready "1 2 3" || bad "4. the fleet did not re-form"
M3=$(until_eq 10 floor mode 3)
[ "$M3" = floor ] && [ "$(gen 3)" = "$G1" ] && ok "4. node 3 took floor at generation $G1 - a member back from away takes the fleet's" \
	|| bad "4. node 3: mode $M3 at $(gen 3) (want floor at $G1)"
A=$(until_eq 20 None alert 1)
[ "$A" = None ] && ok "4. and node 1's cluster.autoscale cleared" || bad "4. node 1 still raises: $A"

echo "--- 5. a resize, then the cool-off: floor holds every table"
R=$(priv 2 resize '{"col": "c", "buckets_log2": 14}')
case "$R" in *'"resizing":true'*) ok "5. resize c 14 accepted through node 2";; *) bad "5. resize refused: $R";; esac
i=0; while [ $i -lt 60 ]; do
	[ "$(bk 1)$(bk 2)$(bk 3)" = 163841638416384 ] && [ "$(busy 1)$(busy 2)$(busy 3)" = 000 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$(bk 1)/$(bk 2)/$(bk 3)" = 16384/16384/16384 ] && ok "5. every table at 16,384 buckets" \
	|| bad "5. buckets $(bk 1)/$(bk 2)/$(bk 3) after the resize (want 16384 on all)"
sleep 8                                  # four cool-offs, no keys
B="$(bk 1)/$(bk 2)/$(bk 3)"
[ "$B" = 16384/16384/16384 ] && ok "5. still 16,384 on all three after 8 s empty - no member shrank it" \
	|| bad "5. buckets $B after the cool-off: a member sized it by its own mode"
grep -h "shrinking to" "$D"/n*.log.* 2>/dev/null | head -3 | sed 's/^/        /'

echo "--- 6. node 2 restarted: the mode comes back from its own file"
F2=$(grep "^autoscale c " "$D/s2/collections" 2>/dev/null)
[ "$F2" = "autoscale c floor $G1" ] && ok "6. node 2's collections file: '$F2'" \
	|| bad "6. node 2's collections file says '$F2' (want 'autoscale c floor $G1')"
L2=$(fleetlines 2)
stop 2
start 2 || bad "6. node 2 did not start"
[ "$(mode 2)" = floor ] && [ "$(gen 2)" = "$G1" ] && ok "6. node 2 back at floor, generation $G1" \
	|| bad "6. node 2 after a restart: $(mode 2) at $(gen 2)"
ready "1 2 3" || bad "6. the fleet did not re-form"
sleep 6                                  # one resync and more: re-sends of what it holds
[ "$(fleetlines 2)" = "$L2" ] && ok "6. the resync's re-sends of the same mode logged nothing ($L2 'set by the fleet' line(s) on node 2, before and after)" \
	|| bad "6. node 2 logged the same mode again: $(fleetlines 2) line(s), $L2 before"

echo "--- 7. a member deaf to the fleet's modes: named as differing"
stop 3
start 3 PERFCACHED_TEST_AUTOSCALE_DEAF=1 || bad "7. node 3 did not start"
ready "1 2 3" || bad "7. the fleet did not re-form"
R=$(priv 1 autoscale '{"col": "c", "mode": "warn"}')
case "$R" in *'"autoscale":"warn"'*) ok "7. autoscale c warn accepted through node 1 (node 3 reads the op)";;
	*) bad "7. autoscale c warn through node 1: $R";; esac
M2=$(until_eq 5 warn mode 2)
[ "$M2/$(mode 3)" = warn/floor ] && ok "7. node 2 took warn; node 3 (deaf) kept floor" \
	|| bad "7. node 2 $M2, node 3 $(mode 3) (want warn, floor)"
A=$(until_eq 15 x alert_has 1 "differ*127.0.20.3")
[ "$A" = x ] && ok "7. node 1 raised cluster.autoscale: $(alert 1 | cut -c1-150)" \
	|| bad "7. no cluster.autoscale on node 1 naming node 3 as differing: $A"
[ "$(differs 1 127.0.20.3)/$(differs 1 127.0.20.2)" = True/False ] \
	&& ok "7. node 1's stats: node 3 autoscale_differs, node 2 not" \
	|| bad "7. node 1's members: node 3 autoscale_differs $(differs 1 127.0.20.3), node 2 $(differs 1 127.0.20.2)"

echo "autoscalefleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
