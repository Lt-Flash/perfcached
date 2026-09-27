#!/bin/sh
# ceilingreplicatest.sh - RV-4c / S244: an eager replica at its arena
# ceiling while its peers keep pushing to it.
#
# Measured first (DESIGN S244): a replica whose arena is full refuses
# every newer copy of a key it already holds - the store needs a cell -
# and goes on answering its OLD copy from memory, a local hit, so nothing
# pulls: 5,000 of 5,000 overwritten keys read back the old value through
# 12 repair cycles.  Now an overwrite that fits the cell it replaces lands
# in place with no carve, and one that does not drops the older copy
# (never a delete: no tombstone, nothing sent) so a read pulls.
#
# Three eager nodes; node 3 has a 16 MB arena, nodes 1 and 2 128 MB.
#   1. 5,000 keys of ~1 KB through node 1; node 3 holds every one
#   2. 35,000 more: node 3 reaches its ceiling (asserted - without it the
#      rest proves nothing), and every key still reads through node 3
#   3. k0000-k2499 overwritten, SAME size: node 3 answers the new value
#      for all 2,500 (in place; counted apart inside nomem)
#   4. k2500-k4999 overwritten, 3 KB: node 3 answers the new value for all
#      2,500 and the old for none; it counts what it dropped
#   5. the drops deleted nothing: node 2 holds all 2,500, node 1 all
#      40,000, and node 3 planted no tombstone
#   6. 30 s of repair cycles later node 3 still serves none of the old
# FAIL-FIRST: rc44 serves the old value in 3 and 4 (5,000 of 5,000).
# Usage: test/ceilingreplicatest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccr.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18661 18662 18663 18671 18672 18673; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "ceilingreplicatest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <arena_mb>
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = $2
[secrets]
client = cr-client-secret
cluster = cr-cluster-secret
[listen]
tcp = 127.0.0.1:1866$1
http = 127.0.0.1:1867$1
plaintext = loopback
[cluster]
multicast = 239.255.77.94:18694
advertise = 127.0.14.$1
mode = eager
collections = c
[collection c]
buckets_log2 = 14
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	return 1
}
# st <n> <python expression over r (the stats) and c (collection c)>
st() { curl -s "http://127.0.0.1:1867$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
# put <n> <from> <to> <tag> <pad>: set k<i> = <tag><i> + pad x's; prints how many stored
put() { python3 - "1866$1" "$2" "$3" "$4" "$5" <<'PY'
import json, socket, sys
port, a, b, tag, pad = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
s = socket.create_connection(("127.0.0.1", port), timeout=60); f = s.makefile("rb"); ok = 0
for x in range(a, b, 200):
    rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c",
          "key": "k%05d" % i, "value": tag + "%05d" % i + "x" * pad}}) for i in range(x, min(b, x + 200))]
    s.sendall(("\n".join(rq) + "\n").encode())
    for _ in rq:
        r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m)
        if (r.get("result") or {}).get("stored"):
            ok += 1
        elif ok == 0 and not getattr(sys, "said", 0):
            sys.said = 1
            sys.stderr.write("   first refusal: %s\n" % json.dumps(r)[:200])
print(ok)
PY
}
# reads <n> <from> <to> <tag>: how many answer exactly <tag><i>...
reads() { python3 - "1866$1" "$2" "$3" "$4" <<'PY'
import json, socket, sys
port, a, b, tag = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
s = socket.create_connection(("127.0.0.1", port), timeout=60); f = s.makefile("rb"); hit = 0
for x in range(a, b, 200):
    rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "k%05d" % i}})
          for i in range(x, min(b, x + 200))]
    s.sendall(("\n".join(rq) + "\n").encode())
    for _ in range(x, min(b, x + 200)):
        m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m); i = m["id"]   # by id: a pulled GET answers late
        v = (m.get("result") or {}).get("value") or ""
        hit += 1 if v.startswith(tag + "%05d" % i) else 0
print(hit)
PY
}

conf 1 128; conf 2 128; conf 3 16
for n in 1 2 3; do start $n || { echo "ceilingreplicatest: node $n did not start: $(tail -3 "$D/n$n.log" | tr '\n' ' ')"; exit 1; }; done
i=0; while [ $i -lt 60 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
		[ "$(st 3 'len(r["cluster"]["peers"])')" = 2 ] && break
	sleep 0.5; i=$((i+1))
done

# 1
put 1 0 5000 A 1000 >/dev/null
sleep 3
N=$(reads 3 0 5000 A)
[ "$N" = 5000 ] && ok "5,000 keys of ~1 KB through node 1: node 3 holds all of them" \
	|| bad "node 3 holds $N of 5,000 before the ceiling"
# 2
put 1 5000 40000 F 1000 >/dev/null
sleep 5
AC=$(st 3 'r["memory"]["at_ceiling"]'); NM=$(st 3 'r["memory"]["nomem"]'); E3=$(st 3 'c["entries"]')
[ "$AC" = True ] && [ "${NM:-0}" -gt 0 ] 2>/dev/null \
	&& ok "35,000 more: node 3 is at its ceiling - $E3 held, $NM carves refused" \
	|| { bad "node 3 did not reach its ceiling (at_ceiling $AC, nomem $NM) - nothing below means anything"; echo "ceilingreplicatest: $pass passed, $fail failed"; exit 1; }
N=$(reads 3 5000 40000 F)
[ "$N" = 35000 ] && ok "every one of the 35,000 still reads through node 3 (pull on miss)" \
	|| bad "reads through node 3: $N of 35,000"
# 3
put 1 0 2500 B 1000 >/dev/null
sleep 5
NB=$(reads 3 0 2500 B); NA=$(reads 3 0 2500 A); IP=$(st 3 'r["memory"]["nomem_inplace"]')
[ "$NB" = 2500 ] && [ "$NA" = 0 ] && [ "${IP:-0}" -gt 0 ] 2>/dev/null \
	&& ok "same-size overwrite of 2,500 held keys: node 3 answers the new value for all ($IP landed in place)" \
	|| bad "same-size overwrite: node 3 answers new $NB, OLD $NA of 2,500 (in place: $IP)"
# 4
put 1 2500 5000 L 3000 >/dev/null
sleep 5
NL=$(reads 3 2500 5000 L); NA=$(reads 3 2500 5000 A)
DR=$(st 3 'r["cluster"]["recv_stale_dropped"]'); RN=$(st 3 'r["cluster"]["recv_nomem"]')
[ "$NL" = 2500 ] && [ "$NA" = 0 ] && [ "${DR:-0}" -gt 0 ] 2>/dev/null \
	&& ok "3 KB overwrite of 2,500 held keys: node 3 answers the new value for all, the old for none ($DR stale copies dropped of $RN refused)" \
	|| bad "3 KB overwrite: node 3 answers new $NL, OLD $NA of 2,500 (dropped $DR, refused $RN)"
M=$(curl -s "http://127.0.0.1:18673/metrics")
echo "$M" | grep -qE '^perfcached_stale_dropped_total [1-9]' && echo "$M" | grep -qE '^perfcached_writes_refused_inplace_total [1-9]' \
	&& ok "/metrics carries both: $(echo "$M" | grep -E '^perfcached_(stale_dropped|writes_refused_inplace)_total' | tr '\n' ' ')" \
	|| bad "/metrics lacks the S244 counters"
# 5
N2=$(reads 2 2500 5000 L); E1=$(st 1 'c["entries"]'); T3=$(st 3 'r["cluster"]["tombstones_retained"]["entries"]')
[ "$N2" = 2500 ] && [ "$E1" = 40000 ] && [ "$T3" = 0 ] \
	&& ok "the drops deleted nothing: node 2 answers all 2,500, node 1 holds 40,000, node 3 planted no tombstone" \
	|| bad "after the drops: node 2 answers $N2 of 2,500, node 1 holds $E1 of 40,000, node 3 tombstones $T3"
# 6
sleep 30
NA=$(reads 3 0 5000 A); NN=$(( $(reads 3 0 2500 B) + $(reads 3 2500 5000 L) ))
[ "$NA" = 0 ] && [ "$NN" = 5000 ] \
	&& ok "30 s of repair cycles later: node 3 serves the old value for none, the new for all 5,000 (cycles $(st 3 'r["cluster"].get("repl_sweep_cycles", "?")'))" \
	|| bad "30 s later node 3 serves OLD $NA, new $NN of 5,000"
echo "ceilingreplicatest: $pass passed, $fail failed"
[ $fail -eq 0 ]
