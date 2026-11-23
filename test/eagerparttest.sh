#!/bin/sh
# eagerparttest.sh - an eager fleet cut by netcutshim.so (eperm and silent), written on both sides,
# healed, stopped, restarted: one value per key, no delete comes back, a newer write beats a delete.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=$(cd "$(dirname "${2:-./netcutshim.so}")" && pwd)/$(basename "${2:-./netcutshim.so}")
D=$(mktemp -d /var/tmp/pcep.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.77.91
MP=17191
[ -f "$SHIM" ] || { echo "eagerparttest: no shim at $SHIM"; exit 1; }
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
tcp = 127.0.0.1:1735$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.13.$1
pull_timeout_ms = 400
mode = eager
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
		ss -ltn 2>/dev/null | grep -qE ":1735[123][[:space:]]" && busy=1
		ss -lun 2>/dev/null | grep -qE ":$MP[[:space:]]" && busy=1
		[ $busy = 0 ] && return 0
		sleep 0.1; i=$((i+1)); done
	echo "    note: the previous fleet is still bound after 15 s: $(ss -ltnp 2>/dev/null | grep -E ':1735[123][[:space:]]' | tr '\n' ' ')"
	return 1
}
alive() { p=$(cat "$D/n$1.pid" 2>/dev/null) || return 1
	s=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null) || return 1
	[ -n "$s" ] && [ "$s" != Z ]; }
st() { # st <n>: "role peers mapterm entries state"
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys, pcnative
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5); f = pcnative.wrap(s)
f.write(sys.stdin.read().encode()); f.flush()
try:
    r = json.loads(f.readline())["result"]; c = r["cluster"]
    e = [x.get("entries") for x in r.get("collections", []) if x.get("name") == "c"]
    print(c.get("role"), c.get("peers_up"), (c.get("map") or {}).get("term", "?"), e[0] if e else "?", r.get("state", "?"))
except Exception: print("? ? ? ? ?")' "1735$1" 2>/dev/null || echo "? ? ? ? ?"; }
# ops <n> <json lines on stdin>: run requests on node n, one connection
ops() { timeout 60 python3 -c '
import json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
reqs = [l for l in sys.stdin.read().splitlines() if l]
for j, l in enumerate(reqs):
    r = json.loads(l); r["jsonrpc"] = "2.0"; r["id"] = j
    f.write((json.dumps(r) + "\n").encode())
f.flush()
errs = [e for e in (json.loads(f.readline()) for _ in reqs) if "error" in e]
if errs: print("    ops: %d of %d requests answered with an error, e.g. %s" % (len(errs), len(reqs), errs[0]["error"].get("message")))' "1735$1"; }
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
# what each node did about the cut, for a red run: its state and the cluster lines that decide a heal
diag() { for n in 1 2 3; do echo "    node $n: $(st $n)"
	grep -hE "rejoin -|resync -|reconcile|rebuild|bootstrapped|is back after|caught up" "$D/n$n.log" | tail -n 6 | cut -c1-200 | sed "s/^/      n$n: /"; done; }
view() { timeout 60 python3 -c '
import hashlib, json, socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
keys = ["m%03d" % i for i in range(200)] + ["i%03d" % i for i in range(100)] + ["a%03d" % i for i in range(300)] + ["n%03d" % i for i in range(20)]
for j, k in enumerate(keys):
    f.write((json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": k}}) + "\n").encode())
f.flush()
val = {}
for _ in keys:
    m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if not (isinstance(_m, dict) and "notify" in _m)); r = m.get("result") or {}
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
print("m=%d i=%d base=%d gone=%d dbl=%d/%s nw=%d" % (m, i_, base, gone, okdbl, dig, nw))' "1735$1" 2>/dev/null || echo "m=? i=? base=? gone=? dbl=?"; }
field() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
cutcount() { cat "$D/cutlog$1" 2>/dev/null | awk '{print $2 + $4 + $6 + $8}'; }

run() { # run <eperm|silent>
	mode=$1; fl=""; [ "$mode" = eperm ] && fl=eperm
	echo "--- $mode: a cut send $( [ "$mode" = eperm ] && echo "fails with EPERM (a local firewall)" || echo "is silently lost (a far cut)")"
	OLD=$(cat "$D"/n?.pid 2>/dev/null | tr '\n' ' ')
	for n in 1 2 3; do stop_kill $n; done
	reap "$OLD"
	rm -rf "$D"/wal? "$D"/cut? "$D"/cutlog? "$D"/n?.log
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
		c=0; for n in 1 2 3; do [ "$(st $n | cut -d' ' -f4)" = 320 ] && c=$((c+1)); done
		[ $c = 3 ] && break; sleep 0.5; i=$((i+1)); done
	[ $c = 3 ] && ok "$mode: 320 base keys on all three" || {
		bad "$mode: base keys did not converge ($c/3)"
		# say what each node held and what it last logged: a 0/3 under
		# ASan on a CI runner is otherwise undiagnosable from the log
		for n in 1 2 3; do echo "    node $n st: $(st $n)"; tail -3 "$D/n$n.log" | sed 's/^/    n'"$n"': /'; done
		return; }

	# ---- cut ----
	echo "cut 127.0.13.1 127.0.13.2 mcast $fl rst" > "$D/cut3"
	echo "cut 127.0.13.3 $fl rst" > "$D/cut1"; echo "cut 127.0.13.3 $fl rst" > "$D/cut2"
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
	# the repair sweep needs no read: before anything is read every node must settle at the same
	# count, 300 + 20 + 200 + 100 less the 50 deleted
	t0=$(date +%s); i=0
	while [ $i -lt 60 ]; do
		e1=$(st 1 | cut -d' ' -f4); e2=$(st 2 | cut -d' ' -f4); e3=$(st 3 | cut -d' ' -f4)
		[ "$e1" = 570 ] && [ "$e2" = 570 ] && [ "$e3" = 570 ] && break
		sleep 1; i=$((i+1))
	done
	[ "$e1" = 570 ] && [ "$e2" = 570 ] && [ "$e3" = 570 ] \
		&& ok "$mode: with NO reads, all three settled at 570 entries in $(( $(date +%s) - t0 )) s - the other side's deletes replayed, none swept back" \
		|| { bad "$mode: with no reads the nodes hold $e1 / $e2 / $e3 (want 570 each: 50 deletes applied everywhere)"; diag; }
	t0=$(date +%s); i=0
	while [ $i -lt 120 ]; do
		v1=$(view 1); v2=$(view 2); v3=$(view 3)
		case "$v1" in "m=200 i=100 base=200 gone=50 dbl=50/"*" nw=20")
			[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && break;; esac
		sleep 0.5; i=$((i+1))
	done
	el=$(( $(date +%s) - t0 ))
	[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && case "$v1" in "m=200 i=100 base=200 gone="*" dbl=50/"*) true;; *) false;; esac \
		&& ok "$mode: healed in ${el} s - every node holds both sides' writes, the untouched keys, and one value per doubly-written key ($v1)" \
		|| { bad "$mode: after ${el} s the fleet disagrees or lost data: node 1 $v1 | node 2 $v2 | node 3 $v3"; diag; }
	G=$(field "$v1" gone); NW=$(field "$v1" nw)
	[ "$G" = 50 ] && [ "$(field "$v2" gone)" = 50 ] && [ "$(field "$v3" gone)" = 50 ] \
		&& ok "$mode: S238 - all 50 deletes made while cut held after the heal, with reads, on every node" \
		|| bad "$mode: S238 - deletes came back: gone $G / $(field "$v2" gone) / $(field "$v3" gone) of 50"
	[ "$NW" = 20 ] && [ "$(field "$v2" nw)" = 20 ] && [ "$(field "$v3" nw)" = 20 ] \
		&& ok "$mode: the isolated side's NEWER writes of the 20 keys the majority deleted survived on every node" \
		|| bad "$mode: a newer write lost to an older delete: nw $NW / $(field "$v2" nw) / $(field "$v3" nw) of 20"
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
print(last)' "1735$M" 2>/dev/null)
	i=0; while [ $i -lt 150 ]; do up=0; for n in 1 2 3; do alive $n && up=$((up+1)); done
		[ $up = 0 ] && break; sleep 0.2; i=$((i+1)); done
	echo "$R" | grep -q '"stopped":\[[0-9]*,[0-9]*\],"still_up":\[\],"lost":\[\]' && [ $up = 0 ] \
		&& ok "$mode: fleetstop through the master (node $M) stopped all three" \
		|| bad "$mode: fleetstop: $up still running; reply $(echo "$R" | cut -c1-120)"
	S=0; for n in 1 2 3; do grep -q "taking the shutdown snapshot" "$D/n$n.log" && S=$((S+1)); done
	for n in 1 2 3; do rm -f "$D/n$n.pid"; done
	start 1 && start 2 && start 3 || { bad "$mode: the fleet did not restart"; return; }
	# a restarted fleet settles, founds and admits before it serves: every node ready, up to 30 s
	python3 "$(dirname "$0")/waitready.py" -t 30 127.0.0.1:17351 127.0.0.1:17352 127.0.0.1:17353 || true
	v1=$(view 1); v2=$(view 2); v3=$(view 3)
	[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && case "$v1" in "m=200 i=100 base=200 gone=$G dbl=50/"*" nw=$NW") true;; *) false;; esac \
		&& ok "$mode: restarted from $S shutdown snapshots + WAL, all three hold what they held before the stop ($v1)" \
		|| bad "$mode: after the restart: node 1 $v1 | node 2 $v2 | node 3 $v3"
	for n in 1 2 3; do stop_kill $n; done
}

run eperm
run silent
echo "eagerparttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
