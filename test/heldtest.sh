#!/bin/sh
# heldtest.sh - S109: a collection reports the bytes it HOLDS, on every
# node, whatever path stored the record.  Two eager nodes; every record is
# written at node 1 and reaches node 2 by the write-path push, so node 2's
# figure comes from records it never took from a client - the case the
# page used to show as a dash.  The figure comes from the maintenance
# thread's paced walk (at least every 5 s here), so every read is an
# await.  Fail-first: a daemon without held_bytes answers MISSING and
# every assertion here fails.
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pchd.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
node() { # node <n> <cliport>  (advertises 127.0.1.2<n>)
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 16
arena_cap_mb = 64
[secrets]
client = hd-client-secret
cluster = hd-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.62:17162
advertise = 127.0.1.2$1
pull_timeout_ms = 300
[collection hd]
buckets_log2 = 12
mode = eager
CONF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
# drive <port> <op> [args]: one connection per call; ops print one value.
#   fill <n> <vlen> [ttl]   -> "sets=<n> requests=<count on this connection>"
#   del <n>                 -> "dels=<n>"
#   held                    -> held_bytes of collection hd, or MISSING
#   entries                 -> entries of hd
#   hist                    -> held_hist of hd as "a,b,c,d,e,f,g,h", or MISSING (S116)
#   cells / index           -> held_cells / index_bytes of hd, or MISSING (S120)
#   live / regions          -> stats.memory.arena_live / arena_regions
#   budget                  -> "index records" from stats.budget, or MISSING
#   hp                      -> this node's own stats headroom_pct (S117)
#   peer_hp                 -> the OTHER member's headroom as this node's
#                              members verb reports it from the beat, in %
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys
port = int(sys.argv[1]); op = sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=10)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
if op == "fill":
    n, vlen = int(sys.argv[3]), int(sys.argv[4])
    ttl = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    done = 0
    for i in range(n):
        p = {"col": "hd", "key": "hk%04d" % i, "value": "v" * vlen}
        if ttl: p["ttl"] = ttl
        r = call("set", **p)
        if "error" not in r: done += 1
    print("sets=%d requests=%d" % (done, rid[0]))
elif op == "del":
    n = int(sys.argv[3]); done = 0
    for i in range(n):
        r = call("del", col="hd", key="hk%04d" % i)
        if "error" not in r: done += 1
    print("dels=%d" % done)
elif op in ("cells", "index"):
    r = call("stats"); r = r.get("result", r)
    for c in r.get("collections", []):
        if c.get("name") == "hd":
            v = c.get("held_cells" if op == "cells" else "index_bytes"); print("MISSING" if v is None else v); sys.exit(0)
    print("MISSING")
elif op in ("live", "regions"):
    r = call("stats"); r = r.get("result", r).get("memory", {})
    v = r.get("arena_live" if op == "live" else "arena_regions"); print("MISSING" if v is None else v)
elif op == "budget":
    r = call("stats"); r = r.get("result", r).get("budget")
    print("MISSING" if not r else "%s %s" % (r.get("index"), r.get("records")))
elif op == "hist":
    r = call("stats"); r = r.get("result", r)
    for c in r.get("collections", []):
        if c.get("name") == "hd":
            v = c.get("held_hist"); print("MISSING" if v is None else ",".join(str(x) for x in v)); sys.exit(0)
    print("MISSING")
elif op == "hp":
    r = call("stats"); r = r.get("result", r).get("memory", {})
    v = r.get("headroom_pct"); print("MISSING" if v is None else v)
elif op == "peer_hp":
    r = call("members"); r = r.get("result", r)
    for m in r.get("members", []):
        if not m.get("self") and m.get("total_mb"):
            print(int(round(100.0 * m["free_mb"] / m["total_mb"]))); sys.exit(0)
    print("MISSING")
else:
    r = call("stats")
    r = r.get("result", r)
    for c in r.get("collections", []):
        if c.get("name") == "hd":
            v = c.get("held_bytes" if op == "held" else "entries")
            print("MISSING" if v is None else v); sys.exit(0)
    print("MISSING")
PYEOF
}
await() { # await <port> <op> <want> <secs>
	i=0
	while [ $i -lt $(( $4 * 10 )) ]; do
		[ "$(drive $1 $3)" = "$2" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
node 1 17621; node 2 17622
start 1; start 2
# both READY before the first write: a node still pulling its bootstrap
# (S83) refuses client writes, and the first fill here once landed in that
# window and stored nothing
for n in 1 2; do
	i=0
	while [ $i -lt 300 ]; do
		grep -q "node state .*-> ready" "$D/n$n.log" && break
		sleep 0.1; i=$((i+1))
	done
	grep -q "node state .*-> ready" "$D/n$n.log" || { echo "node $n never became ready"; tail -5 "$D/n$n.log"; exit 1; }
done

# 1. 200 records of 6-byte keys and 100-byte values at node 1
R=$(drive 17621 fill 200 100)
[ "$R" = "sets=200 requests=200" ] && ok "200 records written at node 1 over one connection" \
	|| bad "fill at node 1: $R"
await 17622 200 entries 20 && ok "node 2 holds all 200 by the push" \
	|| bad "node 2 never reached 200 entries ($(drive 17622 entries))"
await 17621 21200 held 20 && ok "node 1 holds 21200 bytes: 200 x (6 + 100)" \
	|| bad "node 1 held_bytes $(drive 17621 held), want 21200"
await 17622 21200 held 30 && ok "node 2 holds 21200 bytes too - records it never took from a client" \
	|| bad "node 2 held_bytes $(drive 17622 held), want 21200 (the S109 case)"

# S116: the SIZES held come from the same walk as held, so the push-fed
# node shows them too - 200 values of 100 bytes are 200 records in the
# second class (65..256 B).  A daemon whose histogram counts only this
# node's client writes has nothing for node 2; one without held_hist
# answers MISSING.
await 17621 "0,200,0,0,0,0,0,0" hist 20 && ok "node 1's sizes held: 200 records in the 65..256 B class (S116)" \
	|| bad "node 1 held_hist $(drive 17621 hist), want 0,200,0,0,0,0,0,0 (S116)"
await 17622 "0,200,0,0,0,0,0,0" hist 30 && ok "node 2's sizes held match - records it never took from a client (S116)" \
	|| bad "node 2 held_hist $(drive 17622 hist), want 0,200,0,0,0,0,0,0 (S116: sizes counted client writes only)"

# S120: the budget's parts are exact.  The records a collection holds are
# reported as the cells they occupy - the class rounding every record pays -
# and with nothing but walked records in the table that is the arena's own
# live figure, byte for byte (200 x (28 + 6 + 100) = 134 B -> 192 B cells =
# 38400); the index is the regions the table was carved, which on a
# one-collection node is every region there is.  A daemon before S120
# answers MISSING for both.
L=$(drive 17621 live)
await 17621 "$L" cells 20 && ok "node 1's records as cells equal the arena's live figure: $L bytes (S120)" \
	|| bad "node 1 held_cells $(drive 17621 cells) != arena_live $L (S120)"
[ "$(drive 17621 index)" != MISSING ] && [ "$(drive 17621 index)" = "$(drive 17621 regions)" ] \
	&& ok "node 1's index bytes equal the arena's regions: $(drive 17621 regions) (S120)" \
	|| bad "node 1 index_bytes $(drive 17621 index) != arena_regions $(drive 17621 regions) (S120)"
B=$(drive 17621 budget)
[ "$B" = "$(drive 17621 regions) $(drive 17621 cells)" ] && ok "the budget block sums them: $B (S120)" \
	|| bad "budget '$B' != regions + cells (S120)"

# S117: the headroom a member advertises in its beat - what the other
# node's members verb and the fleet cards show - is the CEILING's, the same
# figure the node reports for itself in stats.  Under this elastic shape
# (16 MB reservation under a 64 MB ceiling) the pool's free share and the
# ceiling's headroom differ by tens of points, so a daemon that still
# advertises the pool fails here.  One point of rounding is allowed: the
# beat carries whole MB, stats divides bytes.
hp_agrees() { # hp_agrees <node> <own-port> <other-port>
	own=$(drive $2 hp); seen=$(drive $3 peer_hp)
	[ "$own" != MISSING ] && [ "$seen" != MISSING ] \
		&& [ $(( own - seen )) -le 1 ] && [ $(( seen - own )) -le 1 ] \
		&& ok "node $1 advertises the headroom it reports for itself: seen $seen%, own $own% (S117)" \
		|| bad "node $1 is seen at $seen% headroom but reports $own% for itself (S117: the beat carries the pool's share)"
}
hp_agrees 1 17621 17622
hp_agrees 2 17622 17621

# 2. overwrite 50 with 200-byte values: 150 x 106 + 50 x 206 = 26200
R=$(drive 17621 fill 50 200)
[ "$R" = "sets=50 requests=50" ] || bad "overwrite at node 1: $R"
await 17621 26200 held 20 && ok "an overwrite moves the figure by the size difference (26200)" \
	|| bad "node 1 held_bytes $(drive 17621 held) after overwrite, want 26200"
await 17622 26200 held 30 && ok "node 2 follows the overwrite (26200)" \
	|| bad "node 2 held_bytes $(drive 17622 held) after overwrite, want 26200"

# 3. delete the first 100 (the 50 overwritten among them): 100 x 106 = 10600
R=$(drive 17621 del 100)
[ "$R" = "dels=100" ] || bad "delete at node 1: $R"
await 17622 "0,100,0,0,0,0,0,0" hist 30 && ok "node 2's sizes held follow the delete: 100 records left (S116)" \
	|| bad "node 2 held_hist $(drive 17622 hist) after the delete, want 0,100,0,0,0,0,0,0 (S116)"
await 17621 10600 held 20 && ok "a delete removes the record's bytes (10600)" \
	|| bad "node 1 held_bytes $(drive 17621 held) after delete, want 10600"
await 17622 10600 held 30 && ok "node 2 follows the delete (10600)" \
	|| bad "node 2 held_bytes $(drive 17622 held) after delete, want 10600"

# 4. 30 short-lived records: +3180, then the sweep takes them back.  They
# must outlive a walk (paced at 5 s here): a 1 s life was swept before
# any walk could count it, and the figure went 10600 -> 10600.
R=$(drive 17621 fill 30 100 12)
[ "$R" = "sets=30 requests=30" ] || bad "ttl fill at node 1: $R"
await 17621 13780 held 20 && ok "30 records with a TTL add 3180 (13780)" \
	|| bad "node 1 held_bytes $(drive 17621 held) after ttl fill, want 13780"
await 17621 10600 held 40 && ok "expiry gives them back at node 1 (10600)" \
	|| bad "node 1 held_bytes $(drive 17621 held) after expiry, want 10600"
await 17622 10600 held 40 && ok "and at node 2 (10600)" \
	|| bad "node 2 held_bytes $(drive 17622 held) after expiry, want 10600"

echo "heldtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
