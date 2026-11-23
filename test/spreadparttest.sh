#!/bin/sh
# spreadparttest.sh - a SPREAD fleet (replicas = 2) cut by netcutshim.so, written on both sides,
# healed, stopped, restarted: it settles at exactly K = 2 copies and every node answers alike.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=$(cd "$(dirname "${2:-./netcutshim.so}")" && pwd)/$(basename "${2:-./netcutshim.so}")
D=$(mktemp -d /var/tmp/pcsp.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.77.98
MP=18755
[ -f "$SHIM" ] || { echo "spreadparttest: no shim at $SHIM"; exit 1; }
# a dynamic sanitizer runtime (gcc's check-asan) must come FIRST in the preload list, the shim after
# it, as healtest and syncfailtest do
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"

conf() { # conf <n>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = ep-client-secret
cluster = ep-cluster-secret
enable = ep-enable-secret
[listen]
tcp = 127.0.0.1:1875$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.18.$1
pull_timeout_ms = 400
mode = spread
replicas = 2
collections = c
[collection c]
buckets_log2 = 12
[wal]
dir = $D/wal$1
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n>
	: > "$D/n$1.log"
	LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
stop_kill() { kill -9 "$(cat "$D/n$1.pid" 2>/dev/null)" 2>/dev/null; rm -f "$D/n$1.pid"; }
# reap: the previous fleet must be GONE before the next binds - every listener is SO_REUSEPORT, so a
# start beside a dying daemon splits connections and datagrams with the corpse
reap() { # reap <old pids>
	i=0; while [ $i -lt 150 ]; do
		busy=0
		for p in $1; do s=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)
			[ -n "$s" ] && [ "$s" != Z ] && busy=1; done
		ss -ltn 2>/dev/null | grep -qE ":1875[123][[:space:]]" && busy=1
		ss -lun 2>/dev/null | grep -qE ":$MP[[:space:]]" && busy=1
		[ $busy = 0 ] && return 0
		sleep 0.1; i=$((i+1)); done
	echo "    note: the previous fleet is still bound after 15 s: $(ss -ltnp 2>/dev/null | grep -E ':1875[123][[:space:]]' | tr '\n' ' ')"
	return 1
}
alive() { p=$(cat "$D/n$1.pid" 2>/dev/null) || return 1
	s=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null) || return 1
	[ -n "$s" ] && [ "$s" != Z ]; }
# a node's entries as a number, 0 when its stats did not answer (the loops ask again), so a timed-out
# "?" cannot stop the suite in its arithmetic
nent() { v=$(st "$1" | cut -d' ' -f4); case "$v" in ''|*[!0-9]*) echo 0;; *) echo "$v";; esac; }
st() { # st <n>: "role peers mapterm entries state"
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys, pcnative
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5); f = pcnative.wrap(s)
f.write(sys.stdin.read().encode()); f.flush()
try:
    r = json.loads(f.readline())["result"]; c = r["cluster"]
    e = [x.get("entries") for x in r.get("collections", []) if x.get("name") == "c"]
    print(c.get("role"), c.get("peers_up"), (c.get("map") or {}).get("term", "?"), e[0] if e else "?", r.get("state", "?"))
except Exception: print("? ? ? ? ?")' "1875$1" 2>/dev/null || echo "? ? ? ? ?"; }
# ops <n> <json lines on stdin>: run requests on node n, one connection
ops() { timeout 60 python3 -c '
import json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
reqs = [l for l in sys.stdin.read().splitlines() if l]
for j, l in enumerate(reqs):
    r = json.loads(l); r["jsonrpc"] = "2.0"; r["id"] = j
    f.write((json.dumps(r) + "\n").encode())
f.flush()
# pair each reply with its request by ID: a request parked on a forward is answered
# after the ones behind it
res = {}; errs = []
for _ in reqs:
    m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m)
    if "error" in m: errs.append(m)
    res[m.get("id")] = m.get("result") or {}
if errs: print("    ops: %d of %d requests answered with an error, e.g. %s" % (len(errs), len(reqs), errs[0]["error"].get("message")))
with open(sys.argv[2], "a") as out:
    for j, r in enumerate(reqs):
        a = res.get(j, {})
        q = json.loads(r); p = q["params"]
        ok = a.get("stored") or a.get("deleted") or a.get("removed")
        # an op that was NOT acknowledged may still have landed (a timed-out forward), so it
        # is recorded too, as "maybe"
        out.write(json.dumps([sys.argv[1], q["method"], p["key"], p.get("value")] + ([] if ok else ["maybe"])) + "\n")' "1875$1" "$D/acked"; }
sets() { # sets <prefix> <from> <to> <value-prefix>
	python3 -c '
import json, sys
p, a, b, vp = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
for i in range(a, b):
    print(json.dumps({"method": "set", "params": {"col": "c", "key": "%s%03d" % (p, i), "value": "%s%s%03d" % (vp, p, i)}}))' "$@"; }
dels() { python3 -c '
import json, sys
p, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
for i in range(a, b):
    print(json.dumps({"method": "del", "params": {"col": "c", "key": "%s%03d" % (p, i)}}))' "$@"; }
# view <n>: what node n holds, as "m=<ok> i=<ok> base=<ok> gone=<absent of 50> dbl=<digest>"
view() { timeout 60 python3 -c '
import hashlib, json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
keys = ["m%03d" % i for i in range(200)] + ["i%03d" % i for i in range(100)] + ["a%03d" % i for i in range(300)] + ["n%03d" % i for i in range(20)]
for j, k in enumerate(keys):
    f.write((json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": k}}) + "\n").encode())
f.flush()
val = {}
for _ in keys:
    m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m); r = m.get("result") or {}
    val[keys[m["id"]]] = r.get("value")
m = sum(val["m%03d" % i] == "Mm%03d" % i for i in range(200))
i_ = sum(val["i%03d" % i] == "Ii%03d" % i for i in range(100))
untouched = [i for i in range(50, 300) if not (100 <= i < 125 or 150 <= i < 175)]
base = sum(val["a%03d" % i] == "Ba%03d" % i for i in untouched)
gone = sum(val["a%03d" % i] is None for i in list(range(100, 125)) + list(range(150, 175)))
dbl = [val["a%03d" % i] for i in range(50)]
okdbl = sum(v in ("Ma%03d" % i, "Ia%03d" % i) for i, v in enumerate(dbl))
dig = hashlib.sha1(json.dumps(dbl).encode()).hexdigest()[:10]
nw = sum(val["n%03d" % i] == "In%03d" % i for i in range(20))
print("m=%d i=%d base=%d gone=%d dbl=%d/%s nw=%d" % (m, i_, base, gone, okdbl, dig, nw))' "1875$1" 2>/dev/null || echo "m=? i=? base=? gone=? dbl=?"; }
# check <n1> [n2 n3]: reads all 620 names through each node against what was ACKNOWLEDGED (spread
# refuses writes across the cut); prints "agree=<0|1> wrong=<n> live=<expected live keys>"
check() { timeout 120 python3 - "$D/acked" "$@" <<'CPY'
import json, socket, sys, pcnative
acked = [json.loads(l) for l in open(sys.argv[1])]
nodes = [int(x) for x in sys.argv[2:]]
keys = ["m%03d" % i for i in range(200)] + ["i%03d" % i for i in range(100)] + ["a%03d" % i for i in range(300)] + ["n%03d" % i for i in range(20)]
base = {"a%03d" % i: "Ba%03d" % i for i in range(300)}
base.update({"n%03d" % i: "Bn%03d" % i for i in range(20)})
# per key, the acknowledged ops after the base, by side (port 18751 = side A, 18753 = side B)
ops, maybe = {}, {}
for rec in acked:
    port, m, k, v = rec[:4]
    if k in base and v == base[k] and m == "set":
        continue                                  # the base itself
    (maybe if len(rec) > 4 else ops).setdefault(k, []).append((port, m, v))
def allowed(k):
    o = ops.get(k, [])
    if not o:
        outs = {base.get(k)}
    elif k.startswith("n"):                       # majority deleted FIRST, isolated side wrote later: newer wins
        w = [v for p, m, v in o if m == "set"]
        outs = {w[-1]} if w else {None}
    else:
        outs = set(v if m == "set" else None for p, m, v in o)
    for p, m, v in maybe.get(k, []):              # an unacknowledged op may have landed, or not
        outs.add(v if m == "set" else None)
    return outs                                   # a doubly-written key: either side's, as long as all agree
def read(n):
    f = pcnative.wrap(socket.create_connection(("127.0.0.1", 18750 + n), timeout=60))
    for j, k in enumerate(keys):
        f.write((json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": k}}) + "\n").encode())
    f.flush()
    out = {}
    for _ in keys:
        m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m); out[keys[m["id"]]] = (m.get("result") or {}).get("value")
    return out
if nodes == [0]:                              # expected live keys, from the acks alone
    print(sum(1 for k in keys if None not in allowed(k)), sum(1 for k in keys if allowed(k) != {None}))
    sys.exit(0)
views = {n: read(n) for n in nodes}
agree = all(views[n] == views[nodes[0]] for n in nodes)
wrong = [k for k in keys if views[nodes[0]][k] not in allowed(k)]
live = sum(1 for k in keys if views[nodes[0]][k] is not None) if agree else -1
print("agree=%d wrong=%d live=%d" % (agree, len(wrong), live))
for k in wrong[:4]:
    sys.stderr.write("      %s: read %s, acknowledged allows %s\n" % (k, views[nodes[0]][k], sorted(allowed(k), key=str)))
if not agree:
    d = [k for k in keys if len({views[n][k] for n in nodes}) > 1]
    for k in d[:4]:
        sys.stderr.write("      %s differs: %s\n" % (k, " / ".join("n%d=%s" % (n, views[n][k]) for n in nodes)))
    sys.stderr.write("      %d keys differ between nodes\n" % len(d))
CPY
}
field() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
cutcount() { cat "$D/cutlog$1" 2>/dev/null | awk '{print $2 + $4 + $6 + $8}'; }

run() { # run <eperm|silent>
	mode=$1; fl=""; [ "$mode" = eperm ] && fl=eperm
	echo "--- $mode: a cut send $( [ "$mode" = eperm ] && echo "fails with EPERM (a local firewall)" || echo "is silently lost (a far cut)")"
	OLD=$(cat "$D"/n?.pid 2>/dev/null | tr '\n' ' ')
	for n in 1 2 3; do stop_kill $n; done
	reap "$OLD"
	rm -rf "$D"/wal? "$D"/cut? "$D"/cutlog? "$D"/n?.log "$D"/acked
	for n in 1 2 3; do conf $n; : > "$D/cut$n"; done
	start 1 && sleep 1 && start 2 && start 3 || { bad "$mode: the fleet did not start"; return; }
	# wait for the three to see each other AND be READY (a node seeing both peers can still be
	# RECOVERING and refuse writes); a fixed sleep was not enough under concurrent CI jobs
	FW=120; [ -n "${SANRT:-}" ] && FW=360
	i=0; while [ $i -lt $FW ]; do
		p=0; for n in 1 2 3; do s=$(st $n); [ "$(echo "$s" | cut -d' ' -f2)" = 2 ] && [ "$(echo "$s" | cut -d' ' -f5)" = ready ] && p=$((p+1)); done
		[ $p = 3 ] && break; sleep 0.5; i=$((i+1)); done
	[ $p = 3 ] || echo "    note: the fleet was not READY on all three in $((FW / 2)) s ($(st 1) / $(st 2) / $(st 3))"
	{ sets a 0 300 B; sets n 0 20 B; } | ops 1 || echo "    note: the base write to node 1 did not complete (ops exit $?)"
	i=0; while [ $i -lt 120 ]; do
		c=$(( $(nent 1) + $(nent 2) + $(nent 3) ))
		[ $c = 640 ] && break; sleep 0.5; i=$((i+1)); done
	[ $c = 640 ] && ok "$mode: 320 base keys, two copies each (640 held)" || { bad "$mode: base copies did not converge ($c of 640)"; return; }

	# ---- cut ----
	echo "cut 127.0.18.1 127.0.18.2 mcast $fl rst" > "$D/cut3"
	echo "cut 127.0.18.3 $fl rst" > "$D/cut1"; echo "cut 127.0.18.3 $fl rst" > "$D/cut2"
	i=0; while [ $i -lt 60 ]; do
		s3=$(st 3); s1=$(st 1)
		case "$s3 / $s1" in "master 0 "*" / "*" 1 "*) break;; esac
		sleep 0.5; i=$((i+1)); done
	case "$s3 / $s1" in "master 0 "*" / "*" 1 "*)
		ok "$mode: cut - node 3 alone and its own master ($s3), node 1 sees one peer ($s1)";;
		*) bad "$mode: the cut did not separate the fleet: node 3 '$s3', node 1 '$s1'"; return;; esac
	{ dels n 0 20; sets m 0 200 M; sets a 0 50 M; dels a 100 125; } | ops 1
	{ sets i 0 100 I; sets a 0 50 I; dels a 150 175; sets n 0 20 I; } | ops 3
	sleep 2
	V1=$(view 1); V3=$(view 3)
	[ "$(field "$V1" i)" = 0 ] && [ "$(field "$V3" m)" = 0 ] \
		&& ok "$mode: while cut, neither side holds the other's writes (node 1: $V1; node 3: $V3)" \
		|| bad "$mode: writes crossed the cut (node 1: $V1; node 3: $V3)"
	c1=$(cutcount 1); c3=$(cutcount 3)
	[ "${c1:-0}" -gt 0 ] && [ "${c3:-0}" -gt 0 ] \
		&& ok "$mode: the shim delivered the cut ($c1 calls cut on node 1, $c3 on node 3)" \
		|| bad "$mode: the shim cut nothing (node 1: $c1, node 3: $c3) - this run proves nothing"

	# ---- heal ----
	for n in 1 2 3; do : > "$D/cut$n"; done
	R=$(check 0); LO=${R% *}; HI=${R#* }
	echo "   acknowledged: $(grep -c '"18751"' "$D/acked") ops through node 1, $(grep -c '"18753"' "$D/acked") through node 3, $(grep -c maybe "$D/acked") unacknowledged - $LO to $HI keys should be live"
	# no reads first: repair and reclaim alone must reach K copies per live key, so the total must be
	# even, within 2 x [LO, HI], and settled
	t0=$(date +%s); i=0; prev=
	while [ $i -lt 180 ]; do
		e1=$(nent 1); e2=$(nent 2); e3=$(nent 3)
		T=$((e1 + e2 + e3))
		[ $((T % 2)) = 0 ] && [ $T -ge $((2 * LO)) ] && [ $T -le $((2 * HI)) ] && [ "$T" = "$prev" ] && break
		prev=$T; sleep 2; i=$((i+2))
	done
	# the count can sit even and in range at the heal, before any repair: the reads wait out the repair
	i=0
	while [ $i -lt 200 ]; do
		C=$(check 1 2 3 2>/dev/null)
		case "$C" in "agree=1 wrong=0 live="*) break;; esac
		sleep 1; i=$((i+1))
	done
	C=$(check 1 2 3); EXP=${C##*live=}
	case "$C" in "agree=1 wrong=0 live="*)
		ok "$mode: every node answers every key the same, and as acknowledged - both sides' writes, the acked deletes gone, newer writes over older deletes ($C)";;
		*) bad "$mode: after the heal: $C (want agree=1 wrong=0, live $LO..$HI)";; esac
	i=0; while [ $i -lt 60 ]; do
		T=$(( $(nent 1) + $(nent 2) + $(nent 3) ))
		[ "$T" = $((2 * EXP)) ] 2>/dev/null && break
		sleep 2; i=$((i+1))
	done
	[ "$T" = $((2 * EXP)) ] 2>/dev/null \
		&& ok "$mode: exactly 2 copies of each of the $EXP live keys ($T held), settled in $(( $(date +%s) - t0 )) s with no reads before the count" \
		|| bad "$mode: the fleet holds $T copies of $EXP live keys (want $((2 * EXP)))"
	sleep 3
	S1=$(st 1); S2=$(st 2); S3=$(st 3)
	masters=0; for s in "$S1" "$S2" "$S3"; do case "$s" in master*) masters=$((masters+1));; esac; done
	t1=$(echo "$S1" | cut -d' ' -f3); t2=$(echo "$S2" | cut -d' ' -f3); t3=$(echo "$S3" | cut -d' ' -f3)
	[ $masters = 1 ] && [ "$t1" = "$t2" ] && [ "$t1" = "$t3" ] \
		&& ok "$mode: one master, one map term ($t1) across the fleet" \
		|| bad "$mode: after the heal: node 1 '$S1', node 2 '$S2', node 3 '$S3'"

	# ---- fleet stop, then restart from snapshot + WAL ----
	M=0; for n in 1 2 3; do case "$(st $n)" in master*) M=$n;; esac; done
	R=$(timeout 60 python3 -c '
import json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=50))
for i, (m, p) in enumerate([("enable", {"secret": "ep-enable-secret"}), ("fleetstop", {"timeout_ms": 15000})]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)' "1875$M" 2>/dev/null)
	i=0; while [ $i -lt 150 ]; do up=0; for n in 1 2 3; do alive $n && up=$((up+1)); done
		[ $up = 0 ] && break; sleep 0.2; i=$((i+1)); done
	echo "$R" | grep -q '"stopped":\[[0-9]*,[0-9]*\],"still_up":\[\],"lost":\[\]' && [ $up = 0 ] \
		&& ok "$mode: fleetstop through the master (node $M) stopped all three" \
		|| bad "$mode: fleetstop: $up still running; reply $(echo "$R" | cut -c1-120)"
	S=0; for n in 1 2 3; do grep -q "taking the shutdown snapshot" "$D/n$n.log" && S=$((S+1)); done
	for n in 1 2 3; do rm -f "$D/n$n.pid"; done
	start 1 && start 2 && start 3 || { bad "$mode: the fleet did not restart"; return; }
	i=0; while [ $i -lt 60 ]; do
		C=$(check 1 2 3 2>/dev/null)
		[ "$C" = "agree=1 wrong=0 live=$EXP" ] && break
		sleep 1; i=$((i+1))
	done
	C=$(check 1 2 3)
	[ "$C" = "agree=1 wrong=0 live=$EXP" ] \
		&& ok "$mode: restarted from $S shutdown snapshots + WAL, all three answer as before the stop ($C)" \
		|| bad "$mode: after the restart: $C (want agree=1 wrong=0 live=$EXP)"
	for n in 1 2 3; do stop_kill $n; done
}

run eperm
run silent
echo "spreadparttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
