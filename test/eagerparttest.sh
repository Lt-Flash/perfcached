#!/bin/sh
# eagerparttest.sh - RV-4a: an eager fleet split by a partition, with
# writes on BOTH sides, healed, stopped and restarted.
#
# RV-4's map (DESIGN 12gb): eager x partition had no suite.  It is what
# 245-247 run, and the one time it was exercised - RV-5's fault run, F3,
# by hand with nft - it found S233.  Here three local eager nodes (WAL)
# run under netcutshim.so, which cuts node 3 off from nodes 1 and 2 in
# both directions, UDP and TCP, without root.
#
# Twice, once per way a cut looks to the node:
#   eperm   a cut send fails, as a local firewall's OUTPUT drop does (F3):
#           the isolated master cannot publish, so its map keeps the old
#           term while its claim moves on
#   silent  a cut send "succeeds" and is lost, as on a network cut further
#           away: the isolated master publishes its map under its own term
# Each run:
#   1. 300 base keys a000-a299 on every node;
#   2. cut node 3 (the shim's counters must show it DELIVERED): node 3 is
#      alone, nodes 1+2 see one peer;
#   3. writes on both sides - m000-m199 through node 1, i000-i099 through
#      node 3; a000-a049 overwritten differently on each side; a100-a124
#      deleted through node 1, a150-a174 through node 3; while cut, each
#      side must NOT hold the other's new keys (the cut is real);
#   4. heal: within 60 s every node holds every new key, the base keys
#      nobody touched and the SAME value for each doubly-written key; one
#      master, one map term;
#   4b. the 50 deletes made while cut: S238, OPEN.  They come back - each
#      side's repair sweep re-sends its copy of the other side's deleted
#      keys after the 2 s tombstones have gone, and a heal runs no
#      reconcile (that is armed by a RESTART's replay, B4).  Reported, not
#      failed, until S238 lands; then this line becomes an assertion.
#   5. fleetstop through the master stops all three (S233), and a restart
#      of all three from the stop's snapshots + WAL holds the same.
# Usage: test/eagerparttest.sh [./perfcached] [./netcutshim.so]
set -u
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
	LD_PRELOAD="$SHIM" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
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
alive() { p=$(cat "$D/n$1.pid" 2>/dev/null) || return 1
	s=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null) || return 1
	[ -n "$s" ] && [ "$s" != Z ]; }
st() { # st <n>: "role peers mapterm entries"
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(sys.stdin.read().encode())
try:
    r = json.loads(s.makefile("rb").readline())["result"]; c = r["cluster"]
    e = [x.get("entries") for x in r.get("collections", []) if x.get("name") == "c"]
    print(c.get("role"), c.get("peers_up"), c["map"]["term"], e[0] if e else "?")
except Exception: print("? ? ? ?")' "1735$1" 2>/dev/null || echo "? ? ? ?"; }
# ops <n> <json lines on stdin>: run requests on node n, one connection
ops() { timeout 60 python3 -c '
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
reqs = [l for l in sys.stdin.read().splitlines() if l]
for j, l in enumerate(reqs):
    r = json.loads(l); r["jsonrpc"] = "2.0"; r["id"] = j
    f.write((json.dumps(r) + "\n").encode())
f.flush()
for _ in reqs: f.readline()' "1735$1"; }
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
import hashlib, json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
keys = ["m%03d" % i for i in range(200)] + ["i%03d" % i for i in range(100)] + ["a%03d" % i for i in range(300)]
for j, k in enumerate(keys):
    f.write((json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": k}}) + "\n").encode())
f.flush()
val = {}
for _ in keys:
    m = json.loads(f.readline()); r = m.get("result") or {}
    val[keys[m["id"]]] = r.get("value")
m = sum(val["m%03d" % i] == "Mm%03d" % i for i in range(200))
i_ = sum(val["i%03d" % i] == "Ii%03d" % i for i in range(100))
untouched = [i for i in range(50, 300) if not (100 <= i < 125 or 150 <= i < 175)]
base = sum(val["a%03d" % i] == "Ba%03d" % i for i in untouched)
gone = sum(val["a%03d" % i] is None for i in list(range(100, 125)) + list(range(150, 175)))
dbl = [val["a%03d" % i] for i in range(50)]
okdbl = sum(v in ("Ma%03d" % i, "Ia%03d" % i) for i, v in enumerate(dbl))
dig = hashlib.sha1(json.dumps(dbl).encode()).hexdigest()[:10]
print("m=%d i=%d base=%d gone=%d dbl=%d/%s" % (m, i_, base, gone, okdbl, dig))' "1735$1" 2>/dev/null || echo "m=? i=? base=? gone=? dbl=?"; }
field() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
cutcount() { cat "$D/cutlog$1" 2>/dev/null | awk '{print $2 + $4 + $6}'; }

run() { # run <eperm|silent>
	mode=$1; fl=""; [ "$mode" = eperm ] && fl=eperm
	echo "--- $mode: a cut send $( [ "$mode" = eperm ] && echo "fails with EPERM (a local firewall)" || echo "is silently lost (a far cut)")"
	for n in 1 2 3; do stop_kill $n; done
	rm -rf "$D"/wal? "$D"/cut? "$D"/cutlog? "$D"/n?.log
	for n in 1 2 3; do conf $n; : > "$D/cut$n"; done
	start 1 && sleep 1 && start 2 && start 3 || { bad "$mode: the fleet did not start"; return; }
	sleep 4
	sets a 0 300 B | ops 1
	i=0; while [ $i -lt 40 ]; do
		c=0; for n in 1 2 3; do [ "$(st $n | cut -d' ' -f4)" = 300 ] && c=$((c+1)); done
		[ $c = 3 ] && break; sleep 0.5; i=$((i+1)); done
	[ $c = 3 ] && ok "$mode: 300 base keys on all three" || { bad "$mode: base keys did not converge ($c/3)"; return; }

	# ---- cut ----
	echo "cut 127.0.13.1 127.0.13.2 mcast $fl" > "$D/cut3"
	echo "cut 127.0.13.3 $fl" > "$D/cut1"; echo "cut 127.0.13.3 $fl" > "$D/cut2"
	i=0; while [ $i -lt 60 ]; do
		s3=$(st 3); s1=$(st 1)
		case "$s3 / $s1" in "master 0 "*" / "*" 1 "*) break;; esac
		sleep 0.5; i=$((i+1)); done
	case "$s3 / $s1" in "master 0 "*" / "*" 1 "*)
		ok "$mode: cut - node 3 alone and its own master ($s3), node 1 sees one peer ($s1)";;
		*) bad "$mode: the cut did not separate the fleet: node 3 '$s3', node 1 '$s1'"; return;; esac
	{ sets m 0 200 M; sets a 0 50 M; dels a 100 125; } | ops 1
	{ sets i 0 100 I; sets a 0 50 I; dels a 150 175; } | ops 3
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
	t0=$(date +%s); i=0
	while [ $i -lt 120 ]; do
		v1=$(view 1); v2=$(view 2); v3=$(view 3)
		case "$v1" in "m=200 i=100 base=200 gone="*" dbl=50/"*)
			[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && break;; esac
		sleep 0.5; i=$((i+1))
	done
	el=$(( $(date +%s) - t0 ))
	[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && case "$v1" in "m=200 i=100 base=200 gone="*" dbl=50/"*) true;; *) false;; esac \
		&& ok "$mode: healed in ${el} s - every node holds both sides' writes, the untouched keys, and one value per doubly-written key ($v1)" \
		|| bad "$mode: after ${el} s the fleet disagrees or lost data: node 1 $v1 | node 2 $v2 | node 3 $v3"
	G=$(field "$v1" gone)
	[ "$G" = 50 ] && echo "  note $mode: all 50 deletes made while cut held - S238 looks fixed: make this an assertion" \
		|| echo "  note $mode: S238 (open): $((50 - ${G:-0})) of the 50 deletes made while cut came back after the heal"
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
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=50).makefile("rwb")
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
	sleep 5
	v1=$(view 1); v2=$(view 2); v3=$(view 3)
	[ "$v1" = "$v2" ] && [ "$v1" = "$v3" ] && case "$v1" in "m=200 i=100 base=200 gone=$G dbl=50/"*) true;; *) false;; esac \
		&& ok "$mode: restarted from $S shutdown snapshots + WAL, all three hold what they held before the stop ($v1)" \
		|| bad "$mode: after the restart: node 1 $v1 | node 2 $v2 | node 3 $v3"
	for n in 1 2 3; do stop_kill $n; done
}

run eperm
run silent
echo "eagerparttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
