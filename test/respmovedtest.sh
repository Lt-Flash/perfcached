#!/bin/sh
# respmovedtest.sh - RV-10: `resp_redirect = moved` answers a key this node
# does not own with a genuine -MOVED, and `forward` (the default) still
# answers it.
#
# A stock cluster-aware Redis client routes by CLUSTER SLOTS and refreshes
# that table on -MOVED and on nothing else.  The daemon forwards instead
# (S44) - right for a plain client, and the reason a cluster-aware one
# stays stale for ever after a rebalance: it is never told.  The policy is
# per daemon because the daemon cannot tell the two kinds of client apart.
#
# The same client, both policies, a three-node SHARD fleet each:
#  moved   - 30 one-key writes through node A: a key A does not own gets
#            -MOVED <slot> <ip>:<port>; the slot is the one a Redis client
#            computes itself (CRC16/XMODEM mod 16384, checked here); the
#            write repeated at that address succeeds WITHOUT another
#            redirect (so the address really is the owner) and reads back;
#            the fleet forwarded NOTHING; commands without a key (DBSIZE,
#            KEYS) are never redirected;
#  forward - the same 30 writes through node A all answer +OK, none is
#            redirected, and the fleet's forward counter moved.
# A daemon without the policy refuses the config key, so the moved fleet
# does not start.
# Usage: test/respmovedtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrm.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# run <host> <port> <n> <prefix>: n one-key SETs through host:port, following
#   -MOVED once.  Prints: ok=<n> moved=<n> badslot=<n> loops=<n> readback=<n>
# plain <host> <port> <cmd...>: one command, prints the reply's first line
# fwd <host:port>...: sum of cluster.fwd_sent over the nodes (JSON door)
import json, socket, sys

def crc16(b):                                  # CRC16/XMODEM, as Redis Cluster
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c

class R:
    def __init__(s, host, port):
        s.f = socket.create_connection((host, int(port)), timeout=8).makefile("rwb")
    def cmd(s, *a):
        s.f.write(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x), x) for x in a)).encode()); s.f.flush()
        r = s.f.readline().decode("latin1").strip()
        if r.startswith("$") and not r.startswith("$-1"):
            return "$" + s.f.read(int(r[1:]) + 2)[:-2].decode("latin1")
        return r

what = sys.argv[1]
if what == "run":
    host, port, n, prefix = sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
    a = R(host, port); conns = {}
    okn = moved = badslot = loops = readback = 0
    for i in range(n):
        k = "%s%d" % (prefix, i)
        r = a.cmd("SET", k, "v%d" % i)
        if r.startswith("-MOVED "):
            moved += 1
            _, slot, hp = r.split(" ")
            if int(slot) != crc16(k.encode()) % 16384: badslot += 1
            h, p = hp.rsplit(":", 1)
            c = conns.get(hp) or conns.setdefault(hp, R(h, p))
            r = c.cmd("SET", k, "v%d" % i)
            if r.startswith("-MOVED"): loops += 1
            if c.cmd("GET", k) == "$v%d" % i: readback += 1
        elif a.cmd("GET", k) == "$v%d" % i:
            readback += 1
        if r == "+OK": okn += 1
    print("ok=%d moved=%d badslot=%d loops=%d readback=%d" % (okn, moved, badslot, loops, readback))
elif what == "plain":
    print(R(sys.argv[2], sys.argv[3]).cmd(*sys.argv[4:]))
elif what == "fwd":
    tot = 0
    for hp in sys.argv[2:]:
        h, p = hp.rsplit(":", 1)
        f = socket.create_connection((h, int(p)), timeout=8).makefile("rwb")
        f.write(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n'); f.flush()
        tot += (json.loads(f.readline())["result"].get("cluster") or {}).get("fwd_sent", 0)
    print(tot)
EOF
val() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }

# node <arm> <n> <listen-extra>: 127.0.67.<arm><n>:1842<arm><n>, group .20<6+arm>
node() {
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = rm-client-secret
cluster = rm-cluster-secret
[listen]
tcp = 127.0.67.$1$2:1842$2
plaintext = loopback
$3
[cluster]
multicast = 239.255.77.21$1:1741$1
advertise = 127.0.67.$1$2
pull_timeout_ms = 400
mode = shard
collections = 0
[collection 0]
buckets_log2 = 10
EOF
}
start() {
	"$BIN" -f "$D/n$1$2.conf" > "$D/n$1$2.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "node state .* -> ready" "$D/n$1$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1$2 never reported ready: $(grep -E "ERROR" "$D/n$1$2.log" | tail -1 | cut -c1-160)"
	return 1
}
stop_arm() {
	for p in $PIDS; do grep -qa -- "$D/n$1" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done
	PIDS=""; sleep 0.5
}

# ---- resp_redirect = moved -------------------------------------------
UP=1
for n in 1 2 3; do node 1 $n "resp_redirect = moved"; done
for n in 1 2 3; do start 1 $n || UP=0; done
if [ $UP = 1 ]; then
	sleep 3
	F0=$(python3 "$DRV" fwd 127.0.67.11:18421 127.0.67.12:18422 127.0.67.13:18423)
	R=$(python3 "$DRV" run 127.0.67.11 18421 30 mv)
	F1=$(python3 "$DRV" fwd 127.0.67.11:18421 127.0.67.12:18422 127.0.67.13:18423)
	MV=$(val "$R" moved)
	[ "${MV:-0}" -ge 5 ] && [ "$(val "$R" ok)" = 30 ] && [ "$(val "$R" loops)" = 0 ] && [ "$(val "$R" readback)" = 30 ] \
		&& ok "moved: $MV of 30 keys node A does not own were answered -MOVED; each write succeeded at the named address without a second redirect, and reads back" \
		|| bad "moved: $R (want moved>=5 ok=30 loops=0 readback=30)"
	[ "$(val "$R" badslot)" = 0 ] && [ "${MV:-0}" -ge 1 ] && ok "the slot in every -MOVED is the one a Redis client computes (CRC16 mod 16384)" \
		|| bad "moved: $(val "$R" badslot) redirect(s) named a slot the client would not compute"
	[ "$F1" = "$F0" ] && ok "and the fleet forwarded nothing (fwd_sent $F0 -> $F1): a redirect is not a forward" \
		|| bad "moved: the fleet still forwarded (fwd_sent $F0 -> $F1)"
	K1=$(python3 "$DRV" plain 127.0.67.11 18421 DBSIZE); K2=$(python3 "$DRV" plain 127.0.67.11 18421 KEYS '*')
	case "$K1$K2" in *MOVED*) bad "a command without a key was redirected (DBSIZE: $K1, KEYS: $K2)";;
		*) ok "commands without a key are never redirected (DBSIZE $K1)";; esac
else
	bad "the fleet with resp_redirect = moved did not start - a daemon without the policy refuses the key"
fi
stop_arm 1

# ---- forward (the default) -------------------------------------------
UP=1
for n in 1 2 3; do node 2 $n ""; done
for n in 1 2 3; do start 2 $n || UP=0; done
if [ $UP = 1 ]; then
	sleep 3
	F0=$(python3 "$DRV" fwd 127.0.67.21:18421 127.0.67.22:18422 127.0.67.23:18423)
	R=$(python3 "$DRV" run 127.0.67.21 18421 30 fw)
	F1=$(python3 "$DRV" fwd 127.0.67.21:18421 127.0.67.22:18422 127.0.67.23:18423)
	[ "$(val "$R" moved)" = 0 ] && [ "$(val "$R" ok)" = 30 ] && [ "$(val "$R" readback)" = 30 ] && [ "$F1" -gt "$F0" ] \
		&& ok "forward (default): the same 30 writes through node A all answer +OK, none redirected; the fleet forwarded them (fwd_sent $F0 -> $F1)" \
		|| bad "forward: $R, fwd_sent $F0 -> $F1 (want moved=0 ok=30 readback=30 and the counter up)"
else
	bad "the default fleet did not start"
fi
stop_arm 2

echo "respmovedtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
