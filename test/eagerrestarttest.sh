#!/bin/sh
# eagerrestarttest.sh - S305: an eager node restarted after a crash must
# not answer "absent" for a record it lost.
#
# An eager node keeps the copies it RECEIVES off its WAL (the author
# persists them), so after a kill -9 it replays only what it authored and
# lacks every copy written since its last snapshot.  The authors re-send
# those by the repair sweep - at most 4 MB per peer per collection per
# sweep tick.  Until then the node must ask its peers on a miss.  It did
# not in two cases:
#   1  the per-collection layout ([collection] mode = eager, no [cluster]
#      mode): pull was never enabled for the collection, so the node
#      answered "absent" until the sweep came round (300 of 300 keys
#      read wrong through it, on 0.4.1 and the merged 0.4.2 tree alike)
#   2  the fleet-wide layout with more to repair than one 30 s window
#      carries: S303 made a miss final 30 s after the start, while the
#      sweep was still sending (24,000 keys of 1 KB from one author take
#      ~60 s to re-send: 12,293 of 30,050 held at t+35 s, measured)
# Each part: node 2 authors 50 keys of its own (so it restarts RECOVERED,
# not cold - a cold node pulls a bootstrap before it reports ready), node
# 1 authors the rest; node 2 is killed -9, restarted, and every key is
# read through it - part 1 at once, part 2 after the 30 s window.
# Fail-first: before S305 part 1 reads 300 wrong, part 2 reads the keys
# the sweep had not re-sent yet as absent.
# Usage: test/eagerrestarttest.sh [./perfcached] [part2-keys]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
N2=${2:-24000}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18981 18982 18983 18991 18992 18993; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "eagerrestarttest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcer.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM

conf() { # conf <n> <layout: legacy|fleet>
	rm -rf "$D/s$1"; mkdir -p "$D/s$1/wal"
	if [ "$2" = fleet ]; then
		CL="mode = eager
collections = c"; CM=
	else
		CL=; CM="mode = eager"
	fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 256
[secrets]
client = er-client-secret
cluster = er-cluster-secret
[listen]
tcp = 127.0.0.1:1898$1
http = 127.0.0.1:1899$1
plaintext = loopback
[cluster]
multicast = 239.255.79.43:18990
advertise = 127.0.35.$1
pull_timeout_ms = 400
$CL
[collection c]
buckets_log2 = 16
$CM
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 64
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() {
	k=$(ls "$D"/n$1.log.* 2>/dev/null | wc -l); L="$D/n$1.log.$k"
	"$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$L" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start: $(tail -2 "$L" | tr '\n' ' ')"; return 1
}
killn() { p=$(cat "$D/n$1.pid"); kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$1.pid"; }
stopall() { for n in 1 2 3; do [ -f "$D/n$n.pid" ] && { p=$(cat "$D/n$n.pid"); kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$n.pid"; }; done; }
st() { # st <n> <python expr over r>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18990 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
ent() { st $1 '[c for c in r["collections"] if c["name"]=="c"][0]["entries"]'; }
formed() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'len([p for p in r["cluster"]["peers"] if p.get("up")])')$(st 2 'len([p for p in r["cluster"]["peers"] if p.get("up")])')$(st 3 'len([p for p in r["cluster"]["peers"] if p.get("up")])')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# setk <n> <prefix> <count> <value bytes> [pause s per 500]: prints how
# many stored.  The pause keeps one client inside the WAL ring's drain
# rate: an unpaced 30,000 x 1 KB burst dropped 713 records from node 1's
# WAL, sent it HEALING and had ~2,000 writes refused.
setk() { python3 - "1898$1" "$2" "$3" "$4" "${5:-0}" <<'PY'
import json, pcnative, socket, sys, time
port, pre, n, vb, pause = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
s = socket.create_connection(("127.0.0.1", port), timeout=120); f = pcnative.wrap(s); ok = 0
for x in range(0, n, 500):
    rq = []
    for i in range(x, min(n, x + 500)):
        v = ("%s%06d-" % (pre, i)) * (vb // 8 + 1)
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "%s%06d" % (pre, i), "value": v[:vb]}}))
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}
        ok += 1 if r.get("stored") else 0
    if pause:
        time.sleep(pause)
print(ok)
PY
}
# wrongk <n> <prefix> <count> <value bytes>: prints how many read wrong
wrongk() { python3 - "1898$1" "$2" "$3" "$4" <<'PY'
import json, pcnative, socket, sys
port, pre, n, vb = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = socket.create_connection(("127.0.0.1", port), timeout=300); f = pcnative.wrap(s); bad = 0
for x in range(0, n, 500):
    rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "%s%06d" % (pre, i)}}) for i in range(x, min(n, x + 500))]
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m)
        i = m["id"]; v = ("%s%06d-" % (pre, i)) * (vb // 8 + 1)
        bad += 0 if (m.get("result") or {}).get("value") == v[:vb] else 1
print(bad)
PY
}
spread() { # spread <prefix> <count> <vb> <secs>: every node holds them
	t1=$(( $(date +%s) + $4 ))
	while [ $(date +%s) -lt $t1 ]; do
		[ "$(wrongk 1 $1 $2 $3)$(wrongk 2 $1 $2 $3)$(wrongk 3 $1 $2 $3)" = 000 ] && return 0
		sleep 0.5
	done
	return 1
}
held() { # held <count> <secs>: every node holds that many records
	t1=$(( $(date +%s) + $2 ))
	while [ $(date +%s) -lt $t1 ]; do
		[ "$(ent 1)" = $1 ] && [ "$(ent 2)" = $1 ] && [ "$(ent 3)" = $1 ] && return 0
		sleep 1
	done
	echo "    held $(ent 1)/$(ent 2)/$(ent 3) of $1"; return 1
}

# ---- part 1: the per-collection layout -------------------------------
echo "part 1: per-collection eager, node 2 killed -9 and read at once"
for n in 1 2 3; do conf $n legacy; done
for n in 1 2 3; do start $n || exit 1; done
formed || { bad "fleet did not form"; exit 1; }
[ "$(setk 2 own 50 64)" = 50 ] && [ "$(setk 1 k 300 64)" = 300 ] && spread own 50 64 20 && spread k 300 64 20 \
	&& ok "350 keys on all three nodes" || bad "keys did not reach every node"
killn 2; start 2 || exit 1
# until it is READY a recovering node answers "not READY - retry", an
# error a client can act on; the first READY read is the one that used
# to say "absent"
i=0; while [ "$(st 2 'r["state"]')" != ready ] && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done
w=$(wrongk 2 k 300 64)
[ "$w" = 0 ] && ok "restarted node answers all 300 of node 1's keys at once (holds $(ent 2) of 350)" \
	|| bad "restarted node read $w of 300 keys wrong (holds $(ent 2) of 350)"
w=$(wrongk 2 own 50 64)
[ "$w" = 0 ] && ok "and its own 50 (replayed)" || bad "its own keys: $w of 50 wrong"
stopall

# ---- part 2: fleet-wide, more to repair than the 30 s window ---------
echo "part 2: fleet-wide eager, $N2 keys of 1 KB, read after the 30 s window"
for n in 1 2 3; do conf $n fleet; done
for n in 1 2 3; do start $n || exit 1; done
formed || { bad "fleet did not form"; exit 1; }
s1=$(setk 2 own 50 64); s2=$(setk 1 b $N2 1000 0.25)
[ "$s1$s2" = "50$N2" ] && held $((N2 + 50)) 120 && [ "$(wrongk 3 b $N2 1000)" = 0 ] \
	&& ok "$N2 keys of 1 KB on all three nodes" || bad "keys did not reach every node (stored $s1 + $s2)"
killn 2; start 2 || exit 1; t0=$(date +%s)
i=0; while [ $(( $(date +%s) - t0 )) -lt 35 ] && [ $i -lt 400 ]; do sleep 0.5; i=$((i+1)); done
h=$(ent 2); f=$(st 2 'r["cluster"].get("eager_miss_final")')
echo "    at t+$(( $(date +%s) - t0 ))s node 2 holds $h of $((N2 + 50)), eager_miss_final $f"
w=$(wrongk 2 b $N2 1000)
[ "$w" = 0 ] && ok "after the 30 s window every key reads right through the restarted node" \
	|| bad "after the 30 s window $w of $N2 keys read wrong through the restarted node (it held $h)"
i=0; while [ $i -lt 300 ]; do
	[ "$(st 2 'r["cluster"].get("eager_miss_final")')" = True ] && break
	sleep 1; i=$((i+1))
done
[ "$(st 2 'r["cluster"].get("eager_miss_final")')" = True ] && ok "once repaired ($(ent 2) held, t+$(( $(date +%s) - t0 ))s) its misses are final again" \
	|| bad "eager_miss_final never came back ($(ent 2) held)"
e=$(cat "$D"/n*.log.* | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || { bad "$e ERROR/CRIT lines"; cat "$D"/n*.log.* | grep -E ' (ERROR|CRIT)' | sort | uniq -c | head -5; }
stopall
echo "eagerrestarttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
