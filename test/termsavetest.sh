#!/bin/sh
# termsavetest.sh - a term a node follows but cannot persist is never claimed again: it stands for
# no election until the term is on disk, then claims above it; a failed directory sync fails a store.
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
[ -f "$SHIM" ] || { echo "termsavetest: $SHIM is missing (make syncfailshim.so)"; exit 1; }
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
D=$(mktemp -d /var/tmp/pcterms.XXXXXX)
cleanup() { for n in 1 2 3 4; do kill -9 "$(cat "$D/n$n.pid" 2>/dev/null)" 2>/dev/null; done; rm -rf "$D"; }
trap cleanup EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
W60=60; [ -n "$SANRT" ] && W60=150

conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 16
[secrets]
client = ts-client-secret
cluster = ts-cluster-secret
[listen]
tcp = 127.0.0.1:$((19210 + $1))
http = 127.0.0.1:$((19220 + $1))
plaintext = loopback
[cluster]
multicast = 239.255.79.176:19276
advertise = 127.0.176.$1
[collection 0]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> [armed classes]
	echo "${2:-}" > "$D/ctl$1"
	PC_SYNCFAIL_CTL="$D/ctl$1" PC_SYNCFAIL_LOG="$D/shim$1" LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & echo $! > "$D/n$1.pid"
}
kill9() { kill -9 "$(cat "$D/n$1.pid" 2>/dev/null)" 2>/dev/null; rm -f "$D/n$1.pid"; }
st() { # st <n> <python expression over r, the /stats object>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (19220 + int(sys.argv[1])),timeout=3).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
role() { st "$1" 'r["cluster"]["role"]'; }
state() { st "$1" 'r["state"]'; }
nid() { st "$1" 'r["cluster"]["node"]'; }
mmaster() { st "$1" 'r["cluster"]["map"]["master"]'; }
mterm() { st "$1" 'r["cluster"]["map"]["term"]'; }
unsaved() { st "$1" 'r["cluster"].get("term_unpersisted", "absent")'; }
alerts() { st "$1" '" ".join(a["key"] for a in r["alerts"]["active"])'; }
wait_for() { # wait_for <s> <condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}
# three ready, one master, one map at one term naming a standby that is not the master
formed() {
	st 1 'r["cluster"]["map"]["term"]' > /dev/null || return 1
	f=$(for n in 1 2 3; do st $n '"%s/%s/%s/%s/%s/%s" % (r["state"], r["cluster"]["role"], r["cluster"]["map"]["valid"], r["cluster"]["map"]["term"], r["cluster"]["map"]["backup"], r["cluster"]["map"]["master"])'; done)
	echo "$f" | python3 -c 'import sys
rows = [l.strip().split("/") for l in sys.stdin if l.strip()]
ok = len(rows) == 3 and all(r[0] == "ready" and r[1] in ("member", "master") and r[2] == "True" for r in rows) \
	and sum(r[1] == "master" for r in rows) == 1 and len({r[3] for r in rows}) == 1 \
	and len({(r[4], r[5]) for r in rows}) == 1 and rows[0][4] not in ("0", rows[0][5])
sys.exit(0 if ok else 1)'
}
fleet() { # fleet: a fresh fleet of 1-3; sets M (master), S (its standby), W (the third), T0
	for n in 1 2 3; do kill9 $n; done
	rm -rf "$D"/state1 "$D"/state2 "$D"/state3 "$D"/n1.log "$D"/n2.log "$D"/n3.log "$D"/shim1 "$D"/shim2 "$D"/shim3
	for n in 1 2 3; do conf $n; start $n; done
	wait_for $W60 formed || { echo "   the fleet did not form: $(for n in 1 2 3; do printf '%s ' "$(st $n '"%s/%s/%s" % (r["state"], r["cluster"]["role"], r["cluster"]["map"])')"; done)"; return 1; }
	B=$(st 1 'r["cluster"]["map"]["backup"]')
	M= S= W=
	for n in 1 2 3; do
		if [ "$(role $n)" = master ]; then M=$n
		elif [ "$(nid $n)" = "$B" ]; then S=$n
		else W=$n; fi
	done
	T0=$(mterm $M)
	[ -n "$M" ] && [ -n "$S" ] && [ -n "$W" ]
}
# W, armed, sees S's new term: M is killed and S promotes
follow_unsaved() {
	echo term > "$D/ctl$W"
	kill9 $M
	wait_for $W60 '[ "$(role $S)" = master ] && t=$(mterm $W) && [ "$t" != "?" ] && [ "$t" -gt "$T0" ] && [ "$t" = "$(mterm $S)" ]' \
		|| { echo "   node $S did not take over, or node $W did not follow it: roles $(role $S)/$(role $W), map terms $(mterm $S)/$(mterm $W)"; return 1; }
	T1=$(mterm $S)
}

# ---- 1. the fault held: the node follows and serves, raises the alert, and takes no mastership
echo "--- 1. a term followed but not persisted, the fault held"
if fleet && follow_unsaved; then
	ok "node $W follows node $S's map at term $T1 (was $T0) with its term file refused ($(grep -c '^term ' "$D/shim$W") refusal(s))"
	U=$(unsaved $W)
	[ "$U" = "$T1" ] && ok "its /stats cluster.term_unpersisted names term $T1" \
		|| bad "cluster.term_unpersisted is '$U', not $T1"
	wait_for 15 'alerts $W | grep -q "cluster.term_unpersisted"' \
		&& ok "and it raises cluster.term_unpersisted" \
		|| bad "no cluster.term_unpersisted alert: active '$(alerts $W)'"
	kill9 $S
	wait_for $W60 '[ "$(role $W)" = master ] || grep -q "not taking mastership" "$D/n$W.log"'
	R=$(role $W)
	[ "$R" != master ] && grep -q "not taking mastership" "$D/n$W.log" \
		&& ok "with its master gone it does not take mastership: $(grep -m1 "not taking mastership" "$D/n$W.log" | cut -d' ' -f4- | cut -c1-110)" \
		|| bad "node $W took mastership (role $R, map term $(mterm $W)) holding a term it never persisted"
	: > "$D/ctl$W"
	wait_for $W60 '[ "$(role $W)" = master ] && t=$(mterm $W) && [ "$t" != "?" ] && [ "$t" -gt "$T1" ]' \
		&& ok "the fault lifted, it persists the term and promotes above it: map term $(mterm $W)" \
		|| bad "the fault lifted, node $W is $(role $W) with a map at term $(mterm $W) - it should be master above $T1"
	wait_for 15 '! alerts $W | grep -q "cluster.term_unpersisted"' \
		&& ok "and the alert clears" || bad "the alert stays: '$(alerts $W)'"
else
	bad "the setup failed - the arm tests nothing"
fi

# ---- 2. the fault lifted after its master died, before the election (8 s): no later beacon stores
# the term again, only the node's own retry - the claim is above the followed term, not at it
echo "--- 2. the fault lifted between its master's death and the election"
if fleet && follow_unsaved; then
	kill9 $S
	: > "$D/ctl$W"
	wait_for $W60 '[ "$(role $W)" = master ] && [ "$(mmaster $W)" = "$(nid $W)" ]' \
		|| echo "   node $W did not publish a map as master: role $(role $W), map term $(mterm $W)"
	T2=$(mterm $W)
	[ "$T2" != "?" ] && [ "$T2" -gt "$T1" ] \
		&& ok "node $W promotes at term $T2, above the $T1 node $S published under" \
		|| bad "node $W publishes at term $T2 - the term node $S used for its own maps (it had followed $T1 and persisted $T0)"
else
	bad "the setup failed - the arm tests nothing"
fi
for n in 1 2 3; do kill9 $n; done

# ---- 3. a directory sync refused fails the store: the state files are not called durable
echo "--- 3. the state directory's sync refused from the first start"
conf 4
start 4 dir
wait_for $W60 '[ "$(state 4)" = ready ]' || echo "   node 4 is not ready: $(state 4)"
N=$(grep -c '^dir ' "$D/shim4" 2>/dev/null)
[ "${N:-0}" -ge 1 ] && ok "the fault was delivered ($N directory sync(s) refused)" || bad "the shim refused no directory sync"
[ "$(st 4 'r["cluster"]["identity_durable"]')" = False ] && grep -q "identity is EPHEMERAL" "$D/n4.log" \
	&& ok "the identity is not called durable, and the log says it is ephemeral" \
	|| bad "identity_durable $(st 4 'r["cluster"]["identity_durable"]') - a rename never made durable was taken as stored"
grep -q "mastership term 0 (NOT persisted" "$D/n4.log" \
	&& ok "the term is not called persisted" || bad "the term: $(grep -m1 "mastership term" "$D/n4.log" | cut -d' ' -f4- | cut -c1-100)"
grep -q "could not persist the new cluster id" "$D/n4.log" \
	&& ok "and the cluster id it founded is reported unpersisted" || bad "the founded cluster id was taken as stored"
kill9 4

# ---- 4. the promotion's own claim refused (the followed term is on disk): master, but no map under the
# dead master's term; once the store works it claims above that term and publishes
echo "--- 4. a mastership claim whose term cannot be stored"
if fleet; then
	echo term > "$D/ctl$S"; echo term > "$D/ctl$W"
	kill9 $M
	wait_for $W60 '[ "$(role $S)" = master ] || [ "$(role $W)" = master ]'
	X=$S; [ "$(role $W)" = master ] && X=$W
	if [ "$(role $X)" = master ]; then
		grep -q "could not persist a mastership term" "$D/n$X.log" \
			&& ok "node $X took mastership and says its term could not be persisted" \
			|| bad "node $X is master without saying its claim failed: $(grep -c '^term ' "$D/shim$X" 2>/dev/null) term store(s) refused"
		sleep 12
		[ "$(mmaster $X)" != "$(nid $X)" ] \
			&& ok "12 s on, node $X has published no map of its own (its map: term $(mterm $X), master $(mmaster $X))" \
			|| bad "node $X published a map naming itself master at term $(mterm $X) with its claim refused - the dead master's term $T0, its sequence started again"
		: > "$D/ctl$S"; : > "$D/ctl$W"
		wait_for $W60 '[ "$(mmaster $X)" = "$(nid $X)" ] && t=$(mterm $X) && [ "$t" != "?" ] && [ "$t" -gt "$T0" ]' \
			&& ok "the store works again: node $X claims and publishes at term $(mterm $X), above $T0" \
			|| bad "the fault lifted, node $X's map is at term $(mterm $X) naming master $(mmaster $X) - it should publish its own above $T0"
	else
		bad "neither node $S nor node $W took mastership after node $M was killed"
	fi
else
	bad "the setup failed - the arm tests nothing"
fi
for n in 1 2 3; do kill9 $n; done

echo "termsavetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
