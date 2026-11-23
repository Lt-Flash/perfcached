#!/bin/sh
# gapfloortest.sh - an origin asked for deletes it no longer holds floors the member's mark rather than being
# asked for ever: no hole, no resync, no acks_stuck.  Usage: test/gapfloortest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
export PERFCACHED_TEST_GAP_NO_COPIES=1
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pcgft.XXXXXX)
P1= P2= P3=
trap 'for p in $P1 $P2 $P3; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
gone() { # on a failure: each node whose process has exited, its status and its sanitizer lines
	for i in 1 2 3; do
		eval "p=\$P$i"; [ -n "$p" ] || continue
		case "$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null)" in Z|'') ;; *) continue;; esac
		wait "$p" 2>/dev/null; echo "    node $i (pid $p) is gone, exit status $?"
		grep -hE "runtime error|AddressSanitizer|LeakSanitizer|SUMMARY|^ *#[0-9]+ 0x|fatal|abort" "$D/n$i.log" | head -40 | cut -c1-200 | sed 's/^/      /'
		tail -3 "$D/n$i.log" | cut -c1-200 | sed 's/^/      last: /'
	done
}
done_() { [ $fail -gt 0 ] && gone; [ $fail -gt 0 ] && grep -hE "stuck|floor|no longer holds|rebuild|resync -|reconcile" "$D"/n2.log "$D"/n3.log | grep -v "reconcile dropped" | tail -12 | cut -c1-170 | sed 's/^/    /'; echo "gapfloortest: $pass passed, $fail failed"; [ $fail -eq 0 ]; exit; }
[ -f "$SHIM" ] || { echo "gapfloortest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19641 19642 19643 19649; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "gapfloortest: port $p already bound" >&2; exit 1; }
done
: > "$D/cut2"
ad() { [ "$1" = 1 ] && echo 9 || echo "$1"; }
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = gf-client-secret
cluster = gf-cluster-secret
[listen]
tcp = 127.0.173.$(ad $i):1964$i
plaintext = loopback
[cluster]
discovery = unicast
port = 19649
seeds = 127.0.173.9
advertise = 127.0.173.$(ad $i)
mode = eager
collections = c
tombstone_retain_max = 1000
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
done
start() { # start <i>: node 2 under the shim
	if [ "$1" = 2 ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut2" PC_NETCUT_LOG="$D/cutlog2" \
			"$BIN" -f "$D/n2.conf" >> "$D/n2.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	eval "P$1=\$!"
}
ready() { # ready <i>
	k=0; while [ $k -lt 300 ] && ! grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log"
}
st() { # st <i> <python expr over r = /stats, c = its cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.173.%d" % (9 if i == 1 else i), 19640 + i), 10))
r = pcnative.call(f, "stats")["result"]; c = r["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <node> <set|del> <from> <to> [prefix]: prints how many succeeded
	python3 - "$@" <<'PY'
import socket, sys, pcnative
n, op, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
pfx = sys.argv[5] if len(sys.argv) > 5 else "r"
f = pcnative.wrap(socket.create_connection(("127.0.173.%d" % (9 if n == 1 else n), 19640 + n), 10))
ok = 0
for i in range(a, b):
    k = "%s:%d" % (pfx, i)
    r = pcnative.call(f, "set", col="c", key=k, value="v") if op == "set" else pcnative.call(f, "del", col="c", key=k)
    ok += "error" not in r
print(ok)
PY
}
held() { # held <node> <from> <to>: how many it holds
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.173.%d" % (9 if n == 1 else n), 19640 + n), 10))
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key="r:%d" % i).get("result") or {}).get("found")))
PY
}
short() { st 3 '[(m["confirmed"], m["hi"]) for m in c["acks"]["marks"] if m["confirmed"] < m["hi"]]'; }
stuck() { st "$1" '[a["key"] for a in (r.get("alerts") or {}).get("active", []) if a["key"] == "cluster.acks_stuck"]'; }
start 1; ready 1 || { bad "node 1 did not start: $(tail -2 "$D/n1.log" | tr '\n' ' ')"; done_; }
# node 1 founds alone first: the founder is the lowest identity heard, not the highest address
k=0; while [ $k -lt 150 ] && [ "$(st 1 'c.get("role")')" != master ]; do sleep 0.2; k=$((k+1)); done
T0=$(date +%s)
start 2; start 3
ready 2 && ready 3 || { bad "the members did not start"; done_; }
k=0; while [ $k -lt 150 ] && { [ "$(st 3 'c.get("peers_up")')$(st 2 'c.get("peers_up")')" != 22 ] || [ "$(st 3 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(st 3 'c.get("peers_up")')" = 2 ] && [ "$(st 1 'c.get("role")')" = master ] \
	|| { bad "the fleet did not form with node 1 master: node 1 $(st 1 'c.get("role")'), node 3 peers $(st 3 'c.get("peers_up")')"; done_; }
# a floor's pass drops on the origin's word once it has been up 30 s
w=$((33 - $(date +%s) + T0)); [ $w -gt 0 ] && sleep $w
k=0; while [ $k -lt 75 ]; do [ "$(ops 1 set 0 1)" = 1 ] && break; sleep 0.2; k=$((k+1)); done
S=$(ops 1 set 0 3800); H=$(ops 1 set 0 2 h)
k=0; while [ $k -lt 100 ] && [ "$(held 3 0 3800)" != 3800 ]; do sleep 0.2; k=$((k+1)); done
[ "$S$H" = 38002 ] && [ "$(held 3 0 3800)" = 3800 ] || { bad "setup: node 1 took $S of 3,800, node 3 holds $(held 3 0 3800)"; done_; }
# 0
echo "cut 127.0.173.3 rst" > "$D/cut2"
sleep 0.3
DEL=$(ops 2 del 3300 3800); sleep 1; H3=$(held 3 3300 3800)
: > "$D/cut2"
sleep 0.3
ops 2 del 0 1 h > /dev/null
k=0; while [ $k -lt 120 ] && { [ "$(short)" != "[]" ] || [ "$(held 3 3300 3800)" != 0 ]; }; do sleep 0.5; k=$((k+1)); done
[ "$DEL$H3" = 500500 ] && [ "$(short)" = "[]" ] && [ "$(held 3 3300 3800)" = 0 ] && [ "$(st 2 'c["acks"]["gap_floored"]')" = 0 ] \
	&& ok "0. under the cap node 2 resent the 500 node 3 missed (gap_resent $(st 2 'c["acks"]["gap_resent"]')) and floored nothing" \
	|| { bad "0. the control: node 2 took $DEL deletes, node 3 held $H3 cut and $(held 3 3300 3800) after, mark $(short), gap_floored $(st 2 'c["acks"]["gap_floored"]'), resent $(st 2 'c["acks"]["gap_resent"]')"; done_; }
# 1
echo "cut 127.0.173.3 rst" > "$D/cut2"
sleep 0.3
DEL=$(ops 2 del 0 3000)
[ "$DEL" = 3000 ] || bad "1. node 2 took $DEL of the 3,000 deletes"
sleep 1
H3=$(held 3 0 3000); H1=$(held 1 0 3000)
[ "$H3" = 3000 ] && [ "$H1" = 0 ] && ok "1. node 2 deleted 3,000 of 3,300 cut from node 3, which still holds them all; node 1 none" \
	|| { bad "1. node 3 holds $H3 of the 3,000, node 1 $H1: the cut let them through, or a read pulled"; done_; }
E2=$(st 2 'c["tombstones_retained"]["entries"]'); X2=$(st 2 'c["tombstones_retained"]["dropped_early"]')
[ "$E2" -le 1100 ] 2>/dev/null && [ "$X2" -ge 1900 ] 2>/dev/null \
	&& ok "1. node 2 holds $E2 deletes, the cap dropped $X2: most of node 3's hole is gone from its origin" \
	|| { bad "1. node 2 holds $E2 deletes, dropped $X2: no hole its origin cannot fill"; done_; }
# 2
: > "$D/cut2"
sleep 0.3
[ "$(ops 2 del 1 2 h)" = 1 ] || bad "2. node 2 did not take the delete that names the head"
k=0; L=x
while [ $k -lt 240 ]; do
	L=$(short)
	[ "$L" = "[]" ] && [ "$(st 2 'c["acks"]["gap_floored"]')" -ge 1 ] 2>/dev/null && break
	sleep 0.5; k=$((k+1))
done
[ "$L" = "[]" ] && [ "$(st 2 'c["acks"]["gap_floored"]')" -ge 1 ] 2>/dev/null \
	&& ok "2. node 2 floored node 3's mark (gap_floored $(st 2 'c["acks"]["gap_floored"]')), at the head after $((k / 2)) s" \
	|| bad "2. node 3's mark still short after $((k / 2)) s: $L, gap_floored $(st 2 'c["acks"]["gap_floored"]'), asked $(st 3 'c["acks"]["gap_asked"]'), node 2 resent $(st 2 'c["acks"]["gap_resent"]')"
# 3: the floor opens at a beat; with a survivor it copies that state whole (12ol), and a copying
# node refuses reads, so ready and done first
k=0; while [ $k -lt 240 ] && { [ "$(st 3 'r.get("state")')" != ready ] || [ "$(st 3 'c.get("resyncing")')" != False ] || [ "$(held 3 0 3000)" != 0 ]; }; do sleep 0.5; k=$((k+1)); done
k=0; while [ $k -lt 240 ] && { [ "$(st 3 'c.get("reconcile_open")')" != False ] || [ "$(st 3 'c.get("reconcile_pending")')" != 0 ]; }; do sleep 0.5; k=$((k+1)); done
G3=$(held 3 0 3000); K3=$(held 3 3000 3300); G1=$(held 1 0 3000)
[ "$G3" = 0 ] && [ "$K3" = 300 ] && [ "$G1" = 0 ] \
	&& ok "3. node 3 holds none of the 3,000 deleted keys and all 300 that live; node 1 none" \
	|| bad "3. node 3 holds $G3 of the 3,000 deleted and $K3 of the 300 live, node 1 $G1 of the deleted; rejoins $(st 3 'c.get("rejoins")'), dropped $(st 3 'c.get("reconciled")'), witness $(st 3 'c.get("reconcile_witness")'), skipped $(st 3 'c.get("reconcile_skipped")')"
[ "$(st 3 'c.get("resyncs")')" = 0 ] && [ "$(st 3 'r.get("state")')" = ready ] \
	&& ok "3. no resync: node 3 ready (rejoins $(st 3 'c.get("rejoins")'))" \
	|| bad "3. node 3: resyncs $(st 3 'c.get("resyncs")'), state $(st 3 'r.get("state")')"
# 4
k=0; while [ $k -lt 200 ]; do
	[ "$(stuck 1)$(stuck 2)$(stuck 3)" = "[][][]" ] && break
	sleep 0.5; k=$((k+1))
done
[ "$(stuck 1)$(stuck 2)$(stuck 3)" = "[][][]" ] && ok "4. no cluster.acks_stuck on any node" \
	|| bad "4. cluster.acks_stuck stands: node 1 $(stuck 1), node 2 $(stuck 2), node 3 $(stuck 3)"
done_
