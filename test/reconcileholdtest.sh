#!/bin/sh
# reconcileholdtest.sh - the restart reconcile asks by hold-check (a batch of keys per datagram, held or
# absent with the version) when every live member answers one, and by pull while one does not.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
# the one-by-one reconcile a restart or a floor ran before 12ol is this suite's subject; the default
# now copies a survivor's state whole (rejoinwitnesstest)
export PERFCACHED_TEST_RECONCILE=1
D=$(mktemp -d /var/tmp/pcrhq.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "reconcileholdtest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
for p in 19311 19312 19313 19321 19322 19323 19399; do
	ss -ltnu 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "reconcileholdtest: port $p busy" >&2; exit 1; }
done

conf() {
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = rhq-client-secret
cluster = rhq-cluster-secret
[listen]
tcp = 127.0.0.1:1931$1
http = 127.0.0.1:1932$1
plaintext = loopback
[cluster]
multicast = 239.255.93.94:19399
advertise = 127.0.31.$1
mode = eager
# the open pass is the subject: walk, never the fast path
reconcile_fast = no
# ... and the walk alone: with the marks a restarted member also asks the origin for
# its deletes' tail, applied as deletes and not counted as reconciled
tombstone_acks = no
# ... nor the catch-up: a replay ahead of the walk leaves it nothing to count
tombstone_retain_s = 0
collections = 0
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <readies so far> [env assignment]
	if [ -n "${3:-}" ]; then env "$3" "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & fi
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do
		[ "$(grep -c "perfcached ready" "$D/n$1.log")" -ge "${2:-1}" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; tail -5 "$D/n$1.log"; return 1
}
stop1() { kill "$(cat "$D/n$1.pid")" 2>/dev/null; i=0; while kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done; }
st() { curl -s -m 3 "http://127.0.0.1:1932$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = r["cluster"]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ents() { st $1 '[x for x in r["collections"] if x["name"] == "0"][0]["entries"]'; }
ready3() {
	i=0; while [ $i -lt 200 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'c["peers_up"]')$(st 2 'c["peers_up"]')$(st 3 'c["peers_up"]')" = 222 ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}
safe3() { # a restart of node 3 would be witnessed (S274)
	i=0; while [ $i -lt 90 ] && [ "$(st 3 'c["restart"]["safe"]')" != True ]; do sleep 1; i=$((i+1)); done
	[ "$(st 3 'c["restart"]["safe"]')" = True ]
}
native() { # native <op> <from> <to> - set or del k<from>..k<to-1> through node 1
	python3 - "$@" <<'PY'
import json, pcnative, socket, sys
op, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19311), 10))
for i in range(a, b):
    p = {"col": "0", "key": "k%03d" % i}
    if op == "set":
        p["value"] = "v%03d" % i
    pcnative.call(f, op, **p)
PY
}
passed() { # passed <n> <reconciled wanted> - node n's pass dropped that many, and is closed
	i=0; while [ $i -lt 120 ]; do
		[ "$(st $1 'c.get("reconciled")')" = "$2" ] && [ "$(st $1 'c.get("reconcile_open", False)')" = False ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}

for n in 1 2 3; do conf $n; done
for n in 1 2 3; do start $n 1 || exit 1; done
ready3 || { echo "reconcileholdtest: the fleet did not form"; exit 1; }
native set 0 200
sleep 2.5                                  # the WAL's everysec, on every node
[ "$(ents 3)" = 200 ] || bad "setup: node 3 holds $(ents 3) of 200"
safe3 || { bad "setup: a restart of node 3 never became witnessed"; echo "reconcileholdtest: $pass passed, $fail failed"; exit 1; }

# ---- 1. every member answers hold-checks ----------------------------------
stop1 3
native del 0 50
start 3 2 || exit 1
ready3
if passed 3 50; then
	pr=$(st 3 'c["reconcile_probed"]'); hq=$(st 3 'c.get("reconcile_holdchecks")')
	[ -n "$hq" ] && [ "$hq" != None ] && [ "$hq" -ge 50 ] 2>/dev/null && [ "$hq" = "$pr" ] &&
		ok "1. node 3's walk asked by hold-check: $hq of $pr probed, none by pull" ||
		bad "1. node 3: reconcile_holdchecks '$hq', reconcile_probed '$pr' (want equal, >= 50)"
	[ "$(ents 3)" = 150 ] && ok "1. it dropped exactly the 50 deleted meanwhile and kept 150 (reconciled 50)" ||
		bad "1. node 3 holds $(ents 3) (want 150)"
	s1=$(st 1 'c.get("holdchecks_served")'); s2=$(st 2 'c.get("holdchecks_served")')
	[ "${s1:-0}" -ge 50 ] 2>/dev/null && [ "${s2:-0}" -ge 50 ] 2>/dev/null &&
		ok "1. its peers answered them: holdchecks_served $s1 and $s2" ||
		bad "1. holdchecks_served '$s1' / '$s2' (want >= 50 each)"
else
	bad "1. node 3's pass: reconciled $(st 3 'c.get("reconciled")') (want 50), open $(st 3 'c.get("reconcile_open")'), holds $(ents 3), resyncs $(st 3 'c.get("resyncs")'), skipped $(st 3 'c.get("reconcile_skipped")'), replayed by 1/2 $(st 1 'c.get("tombstones_retained", {}).get("replayed")')/$(st 2 'c.get("tombstones_retained", {}).get("replayed")')"
fi

# ---- 2. a member that does not answer them: the pull ----------------------
stop1 2
start 2 2 PERFCACHED_TEST_NO_HOLDQ=1 || exit 1
ready3
passed 2 0 || bad "2. node 2's own pass did not close"
grep -q 'PERFCACHED_TEST_NO_HOLDQ' "$D/n2.log" || bad "2. node 2 does not say it runs with the test flag"
safe3 || bad "2. a restart of node 3 never became witnessed again"
hq0=$(st 3 'c.get("reconcile_holdchecks")'); pr0=$(st 3 'c["reconcile_probed"]')
stop1 3
native del 50 80
start 3 3 || exit 1
ready3
if passed 3 30; then
	hq=$(st 3 'c.get("reconcile_holdchecks")'); pr=$(st 3 'c["reconcile_probed"]')
	[ "${hq:-x}" = 0 ] && [ "${pr:-0}" -ge 30 ] 2>/dev/null &&
		ok "2. with node 2 answering no hold-check, node 3's walk asked by pull: $pr probed, 0 hold-checks" ||
		bad "2. node 3: reconcile_holdchecks '$hq' (want 0 after its restart), probed '$pr' (want >= 30; before: $hq0 / $pr0)"
	[ "$(ents 3)" = 120 ] && ok "2. and still dropped exactly the 30 deleted meanwhile (holds 120)" ||
		bad "2. node 3 holds $(ents 3) (want 120)"
else
	bad "2. node 3's pass: reconciled $(st 3 'c.get("reconciled")') (want 30), holds $(ents 3)"
fi
echo "reconcileholdtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
