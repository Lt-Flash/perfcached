#!/bin/sh
# waldropacktest.sh - a delete whose WAL record a full ring dropped is confirmed only once a snapshot covers it,
# so the origin keeps it for a crashed member.  Usage: test/waldropacktest.sh [./perfcached] [./syncfailshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pcwda.XXXXXX)
P1= P2=
trap 'for p in $P1 $P2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
done_() { [ $fail -gt 0 ] && grep -hE "dropped|heal|floor|no longer holds|stuck|resync -" "$D"/n1.log "$D"/n2.log | tail -12 | cut -c1-170 | sed 's/^/    /'; echo "waldropacktest: $pass passed, $fail failed"; [ $fail -eq 0 ]; exit; }
[ -f "$SHIM" ] || { echo "waldropacktest: no sync-fail shim at $SHIM" >&2; exit 1; }
for p in 19711 19712 19719; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "waldropacktest: port $p already bound" >&2; exit 1; }
done
ad() { [ "$1" = 1 ] && echo 9 || echo "$1"; }
for i in 1 2; do
	mkdir -p "$D/w$i" "$D/s$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 128
[secrets]
client = wa-client-secret
cluster = wa-cluster-secret
[listen]
tcp = 127.0.174.$(ad $i):1971$i
plaintext = loopback
[cluster]
discovery = unicast
port = 19719
seeds = 127.0.174.9
advertise = 127.0.174.$(ad $i)
mode = eager
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
fsync = everysec
save = off
C
	[ "$i" = 2 ] && printf 'ring_kb = 16\non_full = heal\n' >> "$D/n$i.conf"
	printf '[collection c]\nbuckets_log2 = 12\n' >> "$D/n$i.conf"
	chmod 600 "$D/n$i.conf"
done
: > "$D/ctl"
start() { # start <i>: node 2 under the shim; its log kept across a restart
	if [ "$1" = 2 ]; then
		PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/shim" PC_SYNCFAIL_SLOW_MS=500 PC_SYNCFAIL_DIR_MS=10000 \
			LD_PRELOAD="${SANRT:+$SANRT }$SHIM" "$BIN" -f "$D/n2.conf" >> "$D/n2.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	eval "P$1=\$!"
}
ready() { # ready <i> <how many ready lines>
	k=0; while [ $k -lt 300 ] && [ "$(grep -c "perfcached ready" "$D/n$1.log" 2>/dev/null)" -lt "$2" ]; do sleep 0.1; k=$((k+1)); done
	[ "$(grep -c "perfcached ready" "$D/n$1.log")" -ge "$2" ]
}
st() { # st <i> <python expr over r = /stats, c = its cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.174.%d" % (9 if i == 1 else i), 19710 + i), 10))
r = pcnative.call(f, "stats")["result"]; c = r["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <node> <set|del> <prefix> <n> [retry]: prints how many succeeded; retry = a refused op again, 10 s
	python3 - "$@" <<'PY'
import socket, sys, time, pcnative
n, op, pfx, cnt = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])
retry = len(sys.argv) > 5
f = pcnative.wrap(socket.create_connection(("127.0.174.%d" % (9 if n == 1 else n), 19710 + n), 10))
ok = 0
for i in range(cnt):
    k = "%s:%d" % (pfx, i)
    for _ in range(200 if retry else 1):
        r = pcnative.call(f, "set", col="c", key=k, value="v" * 64) if op == "set" else pcnative.call(f, "del", col="c", key=k)
        if "error" not in r:
            ok += 1
            break
        time.sleep(0.05)
print(ok)
PY
}
held() { # held <node> <prefix> <n>
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, pfx, cnt = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.174.%d" % (9 if n == 1 else n), 19710 + n), 10))
print(sum(1 for i in range(cnt) if (pcnative.call(f, "get", col="c", key="%s:%d" % (pfx, i)).get("result") or {}).get("found")))
PY
}
kept() { st 1 'c["tombstones_retained"]["numbered"]'; }
stuck() { st "$1" '[a["key"] for a in (r.get("alerts") or {}).get("active", []) if a["key"] == "cluster.acks_stuck"]'; }
N=3000
# the setup's writes retry: node 2's 16 KB ring can overflow under load with nothing armed, and a
# healing node refuses writes for the heal's few ms
round() { # round <prefix>: node 2 writes N keys, then node 1 deletes them with node 2's WAL slowed
	S=$(ops 2 set "$1" $N retry)
	k=0; while [ $k -lt 100 ] && [ "$(held 1 "$1" $N)" != $N ]; do sleep 0.2; k=$((k+1)); done
	sleep 2                                  # everysec: node 2's writes synced
	[ "$S" = $N ] && [ "$(held 1 "$1" $N)" = $N ] || { bad "setup $1: node 2 took $S of $N, node 1 holds $(held 1 "$1" $N)"; done_; }
	DR0=$(st 2 'r["wal"]["dropped"]')
	echo "walslow dirslow" > "$D/ctl"
	DEL=$(ops 1 del "$1" $N)
	k=0; while [ $k -lt 100 ] && [ "$(st 2 'r["state"]')" != healing ]; do sleep 0.1; k=$((k+1)); done
	echo dirslow > "$D/ctl"
	DR=$(( $(st 2 'r["wal"]["dropped"]') - DR0 ))
	ST2=$(st 2 'r["state"]')
}

start 1; ready 1 1 || { bad "node 1 did not start: $(tail -2 "$D/n1.log" | tr '\n' ' ')"; done_; }
# node 1 founds alone first: the founder is the lowest identity heard, not the highest address
k=0; while [ $k -lt 150 ] && [ "$(st 1 'c.get("role")')" != master ]; do sleep 0.2; k=$((k+1)); done
start 2; ready 2 1 || { bad "node 2 did not start: $(tail -2 "$D/n2.log" | tr '\n' ' ')"; done_; }
python3 "$T/waitready.py" 127.0.174.9:19711 127.0.174.2:19712 || { bad "a node is not ready"; done_; }
k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 1 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(st 1 'c.get("role")')" = master ] || { bad "node 1 is not the master: $(st 1 'c.get("role")')"; done_; }
# 1
round a
K=$(kept)
[ "$DEL" = $N ] && [ "$DR" -gt 0 ] 2>/dev/null && [ "$ST2" = healing ] && [ "$K" -gt 0 ] 2>/dev/null \
	&& ok "1. node 2 dropped $DR delete records and heals: node 1 keeps $K of the $N deletes for it" \
	|| { bad "1. node 1 took $DEL of $N deletes, node 2 dropped ${DR:-?} records, is ${ST2:-?}, node 1 keeps ${K:-?} (want more than 0 of each, and healing)"; done_; }
# 2
: > "$D/ctl"
k=0; while [ $k -lt 300 ] && { [ "$(st 2 'r["state"]')" != ready ] || [ "$(st 2 'r["wal"]["unhealed"]')" != 0 ] || [ "$(kept)" != 0 ]; }; do sleep 0.2; k=$((k+1)); done
K=$(kept); H2=$(held 2 a $N)
[ "$K" = 0 ] && [ "$H2" = 0 ] && [ "$(st 2 'r["wal"]["heals"]')" -ge 1 ] 2>/dev/null \
	&& ok "2. the heal's snapshot published: node 1 forgot every delete, node 2 holds none of the keys" \
	|| bad "2. after the heal: node 2 $(st 2 'r["state"]'), unhealed $(st 2 'r["wal"]["unhealed"]'), heals $(st 2 'r["wal"]["heals"]'); node 1 keeps $K (want 0), node 2 holds $H2 (want 0)"
# 3
F0=$(st 1 'c["acks"]["gap_floored"]')
round b
K=$(kept); K0=$K
[ "$DR" -gt 0 ] 2>/dev/null && [ "$ST2" = healing ] && [ "$K" -gt 0 ] 2>/dev/null \
	|| { bad "3. node 2 dropped ${DR:-?} records, is ${ST2:-?}, node 1 keeps ${K:-?}: nothing to crash inside"; done_; }
kill -9 "$P2"; wait "$P2" 2>/dev/null
: > "$D/ctl"
start 2; ready 2 2 || { bad "3. node 2 did not restart: $(tail -2 "$D/n2.log" | tr '\n' ' ')"; done_; }
k=0; while [ $k -lt 300 ] && { [ "$(kept)" != 0 ] || [ "$(held 2 b $N)" != 0 ]; }; do sleep 0.5; k=$((k+1)); done
K=$(kept); H2=$(held 2 b $N); H1=$(held 1 b $N); F=$(( $(st 1 'c["acks"]["gap_floored"]') - F0 ))
[ "$K" = 0 ] && [ "$H2" = 0 ] && [ "$H1" = 0 ] && [ "$F" = 0 ] \
	&& ok "3. node 2 killed healing ($DR dropped, node 1 kept $K0): restarted, node 1 gave the deletes back - none floored - and neither node holds a deleted key" \
	|| bad "3. after the restart: node 1 keeps $K (want 0), floored $F (want 0); node 2 holds $H2, node 1 $H1 of the $N deleted keys (want 0)"
k=0; while [ $k -lt 120 ] && [ "$(stuck 1)$(stuck 2)" != "[][]" ]; do sleep 0.5; k=$((k+1)); done
[ "$(stuck 1)$(stuck 2)" = "[][]" ] && ok "3. no cluster.acks_stuck on either node" \
	|| bad "3. cluster.acks_stuck stands: node 1 $(stuck 1), node 2 $(stuck 2)"
done_
