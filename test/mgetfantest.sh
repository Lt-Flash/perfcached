#!/bin/sh
# mgetfantest.sh - G8 (DESIGN 12nh): an mget across owners.  The node a
# client asks reads the keys it holds and looks up the rest the way a get
# does, all at once under one deadline; a key whose node does not answer
# in time is reported as such, never as a miss.  Five three-node fleets,
# collection 0 (RESP's database 0), values "v-<key>":
#  shard   60 keys written through node 1 land on their owners; through
#          node 1, with 5 keys nobody holds:
#          A  native mget: 60 found, 5 absent, every value exact
#          B  RESP MGET: the 60 values and 5 nils, in order
#          C  MGET then PING in one write: the array first, then +PONG -
#             the connection waits for the lookups (pipeline order); and
#             S332, the same for single keys: 60 GETs (the 46 on other
#             nodes each wait on a pull) and a PING in one write come
#             back in order, PING last - before 0.7.2 the PING and every
#             local hit overtook the waiting GETs
#          D  libperfd: perfd_mget and perfd_mget_ex agree (found / absent)
#          E  1,600 keys nobody holds - over 1,000 to look up on other
#             nodes: refused by name, nothing answered as a miss
#          F  node 3 frozen (SIGSTOP; still a member for seconds): its keys
#             come back unanswered, the rest found, in one pull timeout -
#             native and RESP each under twice pull_timeout_ms, not one
#             per key (24 keys: over 7 s), and not on the old 1 s beat
#             (300 ms read as 803-1,103; under 600 on both about one run
#             in eleven); RESP answers -TRYAGAIN naming how many;
#             perfd_mget fails (PERFD_E_REFUSED), perfd_mget_ex says
#             which; counted
#  store   (pull = 1 - without it a get does not ask the fleet either)
#          10 keys written through each node; through node 1 all 30
#          found, and asked again nobody is asked (store keeps the copy).
#          max_pending = 64, both peers frozen so no pull's slot comes
#          back early: an mget whose lookups would leave the pending
#          table below its quarter reserve is refused -TRYAGAIN (busy); a
#          smaller one is taken, and its keys come back unanswered.  (On
#          a fleet that answers, slots come back while the lookups go
#          out - the guard reads the table as each takes its slot - so
#          the 60 are asked and answered: the run before this showed it)
#  proxy   10 keys through each node; through node 1 all 30 found, twice
#          (the second time the locator knows where they are)
#  spread  K = 2: 40 keys through node 1; through each node all 40 found
#  eager   a fresh fleet, not yet settled: an mget of keys nobody holds
#          asks (mget_fanout moves) and answers them absent
# Fail-first: a build without G8 answers the keys on other nodes as
# misses (A, B, D, store, proxy, spread), has no counters (E, F, eager)
# and no perfd_mget_ex (the client does not build).
# Usage: test/mgetfantest.sh [./perfcached] [./mgetextest]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
EX=${2:-./mgetextest}
D=$(mktemp -d /var/tmp/pcmgf.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && { kill -CONT "$p"; kill -9 "$p"; }; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PT=300                                  # pull_timeout_ms

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# fill <port> <prefix> <from> <to>: native sets of <prefix>NNN = v-<key>
# nmget <port> <prefix> <n> <m> [<absent-prefix>]: native mget of n keys
#   and m nobody holds -> found= absent= unanswered= bad= ms=, or err=<text>
# rmget <port> <prefix> <n> <m> [<absent-prefix>]: RESP MGET -> found=
#   nil= bad= ms=, or err=<the error line> ms=
# rpipe <port> <prefix> <n>: RESP MGET + PING in one write -> first= second=
# rgets <port> <prefix> <n>: n RESP GETs + PING in one write -> inorder=
#   (replies matching their GET, counted until the first that does not) last=
# cstat <port> <field>: stats.cluster.<field>;  ent <port>: collection 0's entries
# state <port>: <peers up>/<node state>
import json, pcnative, socket, sys, time

def keys(prefix, n, m, absent="zz"):
    return ["%s%03d" % (prefix, i) for i in range(n)] + \
           ["%s%04d" % (absent, i) for i in range(m)]

def native(port):
    s = socket.create_connection(("127.0.0.1", int(port)), timeout=10)
    return pcnative.wrap(s)

def call(f, method, **p):
    f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
        "params": p}).encode() + b"\n"); f.flush()
    return json.loads(f.readline())

class R:
    def __init__(s, port):
        s.f = socket.create_connection(("127.0.0.1", int(port)), timeout=10).makefile("rwb")
    def send(s, *cmds):
        b = b""
        for a in cmds:
            b += ("*%d\r\n" % len(a)).encode()
            for x in a:
                b += ("$%d\r\n%s\r\n" % (len(x), x)).encode()
        s.f.write(b); s.f.flush()
    def reply(s):
        l = s.f.readline().decode("latin1").rstrip("\r\n")
        if l.startswith("$"):
            n = int(l[1:])
            return None if n < 0 else s.f.read(n + 2)[:-2].decode("latin1")
        if l.startswith("*"):
            return [s.reply() for _ in range(int(l[1:]))]
        return l

w = sys.argv[1]
if w == "fill":
    f = native(sys.argv[2]); st = 0
    for i in range(int(sys.argv[4]), int(sys.argv[5])):
        k = "%s%03d" % (sys.argv[3], i)
        st += bool((call(f, "set", col="0", key=k, value="v-" + k).get("result") or {}).get("stored"))
    print(st)
elif w == "nmget":
    ks = keys(sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), *sys.argv[6:7])
    f = native(sys.argv[2]); t0 = time.time()
    r = call(f, "mget", col="0", keys=ks)
    ms = int((time.time() - t0) * 1000)
    if "result" not in r:
        print("err=%s ms=%d" % ((r.get("error") or {}).get("message", r), ms)); sys.exit()
    v = r["result"]["values"]; fo = ab = un = bd = 0
    for k, e in zip(ks, v):
        if e.get("found"):
            fo += 1; bd += e.get("value") != "v-" + k
        elif e.get("unanswered"):
            un += 1
        else:
            ab += 1
    bd += len(v) != len(ks)
    print("found=%d absent=%d unanswered=%d bad=%d ms=%d" % (fo, ab, un, bd, ms))
elif w == "rmget":
    ks = keys(sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), *sys.argv[6:7])
    c = R(sys.argv[2]); t0 = time.time()
    c.send(["MGET"] + ks); r = c.reply()
    ms = int((time.time() - t0) * 1000)
    if not isinstance(r, list):
        print("err=%s ms=%d" % (r, ms)); sys.exit()
    fo = sum(1 for k, x in zip(ks, r) if x == "v-" + k)
    nil = sum(1 for x in r if x is None)
    print("found=%d nil=%d bad=%d ms=%d" % (fo, nil, len(ks) - fo - nil, ms))
elif w == "rpipe":
    ks = keys(sys.argv[3], int(sys.argv[4]), 0)
    c = R(sys.argv[2]); c.send(["MGET"] + ks, ["PING"])
    a = c.reply(); b = c.reply()
    first = "array%d" % sum(1 for k, x in zip(ks, a) if x == "v-" + k) if isinstance(a, list) else a
    print("first=%s second=%s" % (first, b))
elif w == "rgets":
    ks = keys(sys.argv[3], int(sys.argv[4]), 0)
    c = R(sys.argv[2]); c.send(*([["GET", k] for k in ks] + [["PING"]]))
    rs = [c.reply() for _ in range(len(ks) + 1)]
    n = 0
    for k, x in zip(ks, rs):
        if x != "v-" + k:
            break
        n += 1
    print("inorder=%d last=%s" % (n, rs[-1]))
elif w == "cstat":
    print((call(native(sys.argv[2]), "stats")["result"].get("cluster") or {}).get(sys.argv[3], "missing"))
elif w == "state":
    try:
        r = call(native(sys.argv[2]), "stats")["result"]
        print("%s/%s" % ((r.get("cluster") or {}).get("peers_up"), r.get("state")))
    except Exception as e:
        print("down/%s" % type(e).__name__)
elif w == "ent":
    r = call(native(sys.argv[2]), "stats", col="0")["result"]
    print(r["collections"][0]["entries"])
EOF
val() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }

# node <arm> <n> <cluster lines> <collection lines> - 127.0.91.<arm><n>,
# port 169<arm><n>
node() {
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = mgf-client-secret
cluster = mgf-cluster-secret
[listen]
tcp = 127.0.0.1:169$1$2
plaintext = loopback
[cluster]
multicast = 239.255.91.$1:1797$1
advertise = 127.0.91.$1$2
pull_timeout_ms = $PT
collections = 0
$3
[collection 0]
buckets_log2 = 10
$4
EOF
}
start() { # start <arm> <n>
	"$BIN" -f "$D/n$1$2.conf" > "$D/n$1$2.log" 2>&1 &
	PIDS="$PIDS $!"
	eval "PID$1$2=\$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1$2 did not start: $(grep -E "ERROR" "$D/n$1$2.log" | tail -1 | cut -c1-160)"
	return 1
}
fleet() { # fleet <arm> <cluster lines> [<collection lines>]
	for n in 1 2 3; do node "$1" $n "$2" "${3:-}"; done
	for n in 1 2 3; do start "$1" $n || return 1; done
	# a node takes data only once READY (a joiner recovers first, and its
	# peers' pulls wait on it), and each must see the other two
	i=0
	while [ $i -lt 150 ]; do
		r="$(python3 "$DRV" state 169${1}1) $(python3 "$DRV" state 169${1}2) $(python3 "$DRV" state 169${1}3)"
		[ "$r" = "2/ready 2/ready 2/ready" ] && return 0
		sleep 0.2; i=$((i + 1))
	done
	echo "  arm $1: the fleet never formed: $r"
	return 1
}
stop_arm() {
	for p in $PIDS; do grep -qa -- "$D/n$1" /proc/$p/cmdline 2>/dev/null && { kill -CONT "$p"; kill -9 "$p"; }; done
	PIDS=""; sleep 0.3
}

# ---- shard ---------------------------------------------------------------
if fleet 1 "mode = shard"; then
	S=$(python3 "$DRV" fill 16911 sk 0 60)
	E1=$(python3 "$DRV" ent 16911); E2=$(python3 "$DRV" ent 16912); E3=$(python3 "$DRV" ent 16913)
	[ "$S" = 60 ] && [ $((E1 + E2 + E3)) = 60 ] && [ "$E1" -lt 60 ] && [ "$E3" -gt 0 ] &&
		ok "shard: 60 written through node 1, held $E1 / $E2 / $E3 - $((60 - E1)) on other nodes" ||
		bad "shard: the fill: stored $S, held $E1 / $E2 / $E3"
	R=$(python3 "$DRV" nmget 16911 sk 60 5)
	[ "$(val "$R" found)" = 60 ] && [ "$(val "$R" absent)" = 5 ] && [ "$(val "$R" unanswered)" = 0 ] && [ "$(val "$R" bad)" = 0 ] &&
		ok "A. native mget through node 1: 60 found (exact), 5 absent ($(val "$R" ms) ms)" ||
		bad "A. native mget: $R"
	R=$(python3 "$DRV" rmget 16911 sk 60 5)
	[ "$(val "$R" found)" = 60 ] && [ "$(val "$R" nil)" = 5 ] && [ "$(val "$R" bad)" = 0 ] &&
		ok "B. RESP MGET: the 60 values and 5 nils, in order" || bad "B. RESP MGET: $R"
	R=$(python3 "$DRV" rpipe 16911 sk 60)
	[ "$R" = "first=array60 second=+PONG" ] &&
		ok "C. MGET then PING in one write: the array first, then +PONG" || bad "C. pipeline order: $R"
	R=$(python3 "$DRV" rgets 16911 sk 60)
	[ "$R" = "inorder=60 last=+PONG" ] &&
		ok "C. S332: 60 GETs ($((60 - E1)) waiting on other nodes) and a PING in one write: every value in order, PING last" ||
		bad "C. S332, GETs then PING in one write: $R"
	R=$("$EX" 16911 0 sk 60 5)
	[ "$(echo "$R" | cut -d'|' -f1 | tr -s ' ')" = "mget rc=0 kind=0 found=60 bad=0 " ] &&
		echo "$R" | grep -q "ex rc=0 found=60 absent=5 unanswered=0 bad=0" &&
		ok "D. libperfd: perfd_mget 60 found; perfd_mget_ex 60 found, 5 absent" || bad "D. libperfd: $R"
	F0=$(python3 "$DRV" cstat 16911 mget_fanout)
	[ "${F0:-0}" != missing ] && [ "${F0:-0}" -ge 4 ] &&
		ok "the node counted $F0 mgets that waited on other nodes" || bad "mget_fanout: $F0"
	R=$(python3 "$DRV" nmget 16911 cp 0 1600 cp)
	RF=$(python3 "$DRV" cstat 16911 mget_refused)
	case "$R" in "err=mget needs more than 1000 remote lookups - split it "*)
		[ "$RF" = 1 ] && ok "E. 1,600 keys, over 1,000 on other nodes: refused by name, counted" ||
			bad "E. refused, but mget_refused = $RF";;
		*) bad "E. the cap: $R";; esac

	# F: node 3 frozen - still a member for seconds, answering nothing
	kill -STOP "$PID13"
	T0=$(date +%s%N)
	RN=$(python3 "$DRV" nmget 16911 sk 60 0)
	RR=$(python3 "$DRV" rmget 16911 sk 60 0)
	RX=$("$EX" 16911 0 sk 60 0)
	T1=$(date +%s%N)
	kill -CONT "$PID13"
	echo "  (node 3 frozen for $(( (T1 - T0) / 1000000 )) ms; it holds $E3 of the keys)"
	[ "$(val "$RN" unanswered)" = "$E3" ] && [ "$(val "$RN" found)" = $((60 - E3)) ] && [ "$(val "$RN" bad)" = 0 ] &&
		[ "$(val "$RN" ms)" -lt $((PT * 2)) ] &&
		ok "F. native: node 3's $E3 keys unanswered, the other $((60 - E3)) found, in $(val "$RN" ms) ms (pull_timeout_ms $PT)" ||
		bad "F. native with node 3 frozen: $RN (node 3 holds $E3)"
	case "$RR" in "err=-TRYAGAIN mget: $E3 key(s) not answered "*)
		[ "$(val "$RR" ms)" -lt $((PT * 2)) ] &&
			ok "F. RESP: -TRYAGAIN mget: $E3 key(s) not answered - no values beside nils ($(val "$RR" ms) ms)" ||
			bad "F. RESP answered, but in $(val "$RR" ms) ms - pull_timeout_ms is $PT";;
		*) bad "F. RESP with node 3 frozen: $RR";; esac
	echo "$RX" | grep -q "^mget rc=-1 kind=3 found=0 bad=0 | ex rc=0 found=$((60 - E3)) absent=0 unanswered=$E3 bad=0$" &&
		ok "F. libperfd: perfd_mget fails (PERFD_E_REFUSED), perfd_mget_ex: $E3 unanswered, $((60 - E3)) found" ||
		bad "F. libperfd with node 3 frozen: $RX"
	U=$(python3 "$DRV" cstat 16911 mget_unanswered)
	[ "${U:-0}" != missing ] && [ "${U:-0}" -ge 3 ] && ok "F. counted: mget_unanswered $U" || bad "F. mget_unanswered: $U"
else
	bad "shard: the fleet did not start"
fi
stop_arm 1

# ---- store ---------------------------------------------------------------
if fleet 2 "mode = store
max_pending = 64" "pull = 1"; then
	S=$(( $(python3 "$DRV" fill 16921 st 0 10) + $(python3 "$DRV" fill 16922 st 10 20) + $(python3 "$DRV" fill 16923 st 20 30) ))
	E1=$(python3 "$DRV" ent 16921)
	[ "$S" = 30 ] && [ "$E1" = 10 ] && ok "store: 10 written through each node, node 1 holds its own $E1" ||
		bad "store: the fill: stored $S, node 1 holds $E1"
	R=$(python3 "$DRV" nmget 16921 st 30 3)
	F1=$(python3 "$DRV" cstat 16921 mget_fanout)
	R2=$(python3 "$DRV" rmget 16921 st 30 0)
	F2=$(python3 "$DRV" cstat 16921 mget_fanout)
	[ "$(val "$R" found)" = 30 ] && [ "$(val "$R" absent)" = 3 ] && [ "$(val "$R" bad)" = 0 ] &&
		ok "store: through node 1 all 30 found, 3 absent" || bad "store: native mget: $R"
	[ "$(val "$R2" found)" = 30 ] && [ "$F2" = "$F1" ] &&
		ok "store: asked again, all 30 answered from node 1's copies - nobody asked (mget_fanout stays $F1)" ||
		bad "store: the second mget: $R2, mget_fanout $F1 -> $F2"
	kill -STOP "$PID22" "$PID23"
	R=$(python3 "$DRV" rmget 16921 st 0 60 sg)
	R2=$(python3 "$DRV" rmget 16921 st 0 40 sh)
	kill -CONT "$PID22" "$PID23"
	case "$R" in "err=-TRYAGAIN mget: the cluster's pending table is busy - retry, or split the mget "*)
		ok "store: 60 lookups, 64 pending slots, peers frozen: refused -TRYAGAIN, a quarter kept for everything else ($(val "$R" ms) ms)";;
		*) bad "store: the pending-slot guard: $R";; esac
	case "$R2" in "err=-TRYAGAIN mget: 40 key(s) not answered "*)
		ok "store: 40 lookups fit beside the reserve: taken, and unanswered by the frozen peers";;
		*) bad "store: 40 lookups with the peers frozen: $R2";; esac
else
	bad "store: the fleet did not start"
fi
stop_arm 2

# ---- proxy ---------------------------------------------------------------
if fleet 3 "mode = proxy"; then
	S=$(( $(python3 "$DRV" fill 16931 px 0 10) + $(python3 "$DRV" fill 16932 px 10 20) + $(python3 "$DRV" fill 16933 px 20 30) ))
	E1=$(python3 "$DRV" ent 16931)
	R=$(python3 "$DRV" nmget 16931 px 30 2)
	R2=$(python3 "$DRV" nmget 16931 px 30 0)
	[ "$S" = 30 ] && [ "$E1" -lt 30 ] && [ "$(val "$R" found)" = 30 ] && [ "$(val "$R" absent)" = 2 ] && [ "$(val "$R" bad)" = 0 ] &&
		ok "proxy: node 1 holds $E1 of 30; through it all 30 found, 2 absent" ||
		bad "proxy: stored $S, node 1 holds $E1, mget $R"
	[ "$(val "$R2" found)" = 30 ] && ok "proxy: again, the locator steering: all 30 found" || bad "proxy: the second mget: $R2"
else
	bad "proxy: the fleet did not start"
fi
stop_arm 3

# ---- spread --------------------------------------------------------------
if fleet 4 "mode = spread
replicas = 2" "pull = 1"; then
	S=$(python3 "$DRV" fill 16941 sp 0 40)
	sleep 1
	E="$(python3 "$DRV" ent 16941) $(python3 "$DRV" ent 16942) $(python3 "$DRV" ent 16943)"
	R="$(python3 "$DRV" nmget 16941 sp 40 0) / $(python3 "$DRV" nmget 16942 sp 40 0) / $(python3 "$DRV" nmget 16943 sp 40 0)"
	[ "$S" = 40 ] && [ "$(echo "$R" | grep -o "found=40 absent=0 unanswered=0 bad=0" | wc -l)" = 3 ] &&
		ok "spread K=2: held $E, through each node all 40 found" || bad "spread: stored $S, held $E: $R"
else
	bad "spread: the fleet did not start"
fi
stop_arm 4

# ---- eager, not yet settled -------------------------------------------------
if fleet 5 "mode = eager" "pull = 1"; then
	F0=$(python3 "$DRV" cstat 16951 mget_fanout)
	R=$(python3 "$DRV" nmget 16951 eg 0 10)
	F1=$(python3 "$DRV" cstat 16951 mget_fanout)
	[ "$(val "$R" absent)" = 10 ] && [ "$F1" = $((F0 + 1)) ] &&
		ok "eager, not settled: 10 keys nobody holds - the node asked (mget_fanout $F0 -> $F1), absent" ||
		bad "eager: $R, mget_fanout $F0 -> $F1"
else
	bad "eager: the fleet did not start"
fi
stop_arm 5

echo "mgetfantest: $pass passed, $fail failed"
[ $fail -eq 0 ]
