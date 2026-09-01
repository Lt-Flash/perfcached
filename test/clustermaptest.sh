#!/bin/sh
# clustermaptest.sh — the CLUSTER topology surface on the RESP door (S44).
#
# Placement runs rendezvous over the Redis SLOT, which is what makes this
# surface derivable at all.  Four things must hold or a stock Redis
# client routes wrongly and silently:
#
#  - CLUSTER KEYSLOT agrees with Redis's own CRC16, hash tags included.
#    The test recomputes it here rather than trusting src/pc_slot.h, so
#    a matching pair of bugs on both sides cannot pass.
#  - CLUSTER SLOTS covers all 16384 slots EXACTLY once: a gap is a key
#    no client can place, an overlap is two clients disagreeing.
#  - CLUSTER SHARDS describes the SAME assignment.  Clients pick one
#    reply or the other by version, so a divergence sends half the
#    ecosystem to the wrong node.
#  - a node with no cluster still answers KEYSLOT (it is pure maths) and
#    refuses SLOTS rather than inventing a one-node map.
#
# Run at 2, 3 AND 4 nodes.  The count is not incidental: rendezvous
# scatters ownership, so the SLOTS reply grows with the fleet, and at 4
# nodes it passes 1MB.  This test was written at 3 - the last size that
# fit in the old fixed reply buffer - and so it passed while CLUSTER
# SLOTS was already broken for every real fleet.  Do not reduce the
# counts.
#
# Usage: test/clustermaptest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccm.XXXXXX)
P1= P2= P3= P4= P0=
trap 'for v in "$P1" "$P2" "$P3" "$P4" "$P0"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
      done; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

node() { # node <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = cm-client-secret
cluster = cm-cluster-secret
[listen]
tcp = 127.0.0.7$1:1745$1
resp = 127.0.0.7$1:1746$1
plaintext = loopback
[cluster]
multicast = 239.255.77.56:17156
advertise = 127.0.0.7$1
mode = shard
collections = cm
[collection cm]
buckets_log2 = 12
EOF
}
start() { # start <n>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

PY="$D/cl.py"
cat > "$PY" <<'EOF'
import socket, sys
CRLF = b"\r\n"

def crc16(d):                      # CRC16-CCITT/XMODEM, Redis's own
    c = 0
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def refslot(k):
    b = k.encode()
    i = b.find(b"{")
    if i >= 0:
        j = b.find(b"}", i + 1)
        if j > i + 1:
            b = b[i + 1:j]
    return crc16(b) % 16384

def call(host, port, args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    s = socket.create_connection((host, port), 8)
    s.settimeout(20)
    s.sendall(out)
    buf = b""
    try:
        while True:
            d = s.recv(1 << 20)
            if not d:
                break
            buf += d
            s.settimeout(0.5)
    except socket.timeout:
        pass
    s.close()
    return buf

def dec(x):                            # bytes-or-str -> str, always
    return x.decode() if isinstance(x, bytes) else str(x)

def parse(buf, pos=0):
    t = buf[pos:pos + 1]
    e = buf.index(CRLF, pos)
    head = buf[pos + 1:e]
    pos = e + 2
    if t in (b":",):
        return int(head), pos
    if t in (b"+", b"-"):
        return head.decode(), pos
    if t == b"$":
        n = int(head)
        if n < 0:
            return None, pos
        return buf[pos:pos + n], pos + n + 2
    if t == b"*":
        v = []
        for _ in range(int(head)):
            x, pos = parse(buf, pos)
            v.append(x)
        return v, pos
    raise ValueError("bad type %r" % t)

host, port, what = sys.argv[1], int(sys.argv[2]), sys.argv[3]

if what == "keyslot":
    bad = 0
    for k in ["foo", "bar", "hello", "somekey", "user:{42}:name", "{42}",
              "a{}b", "x{", "{}", "", "user:1000", "{a}{b}"]:
        r, _ = parse(call(host, port, [b"CLUSTER", b"KEYSLOT", k.encode()]))
        if r != refslot(k):
            bad += 1
            print("  keyslot %r: got %r want %d" % (k, r, refslot(k)))
    print("MISMATCHES %d" % bad)

elif what == "cover":
    # SLOTS first, and report it fully before touching SHARDS: a
    # malformed SHARDS reply must not take the SLOTS verdict down with
    # it, or every fault reads as "the fleet never formed".
    sl, _ = parse(call(host, port, [b"CLUSTER", b"SLOTS"]))
    cov = [0] * 16384
    own = {}
    for a, b, node in [(r[0], r[1], r[2]) for r in sl]:
        for s in range(a, b + 1):
            cov[s] += 1
            own[s] = (node[0], node[1])
    print("NODES %d" % len(set(own.values())))
    print("PORTS %s" % ",".join(sorted(set(
        "%s:%s" % (dec(ip), pt) for ip, pt in own.values()))))
    print("ONCE %d GAPS %d OVERLAP %d RANGES %d" %
          (sum(1 for c in cov if c == 1), sum(1 for c in cov if c == 0),
           sum(1 for c in cov if c > 1), len(sl)))
    try:
        sh, _ = parse(call(host, port, [b"CLUSTER", b"SHARDS"]))
        o2 = {}
        for shd in sh:
            d = dict(zip(shd[0::2], shd[1::2]))
            n = dict(zip(d[b"nodes"][0][0::2], d[b"nodes"][0][1::2]))
            ss = d[b"slots"]
            for i in range(0, len(ss), 2):
                for s in range(ss[i], ss[i + 1] + 1):
                    o2[s] = (n[b"ip"], n[b"port"])
        print("DISAGREE %d SHARDS %d" %
              (sum(1 for s in range(16384) if own.get(s) != o2.get(s)), len(sh)))
    except Exception as e:
        # an unreadable reply is a failure of SHARDS, not of everything
        print("DISAGREE -1 SHARDS 0 (%s: %s)" % (type(e).__name__, e))
    try:
        nd, _ = parse(call(host, port, [b"CLUSTER", b"NODES"]))
        # a status/error reply parses to str: that IS the finding -
        # surface it verbatim instead of tripping over .decode later
        if not isinstance(nd, bytes):
            print("NODESCMD -1 -1 (reply was not a bulk: %r)" % (nd,))
        else:
            o3 = {}
            for ln in dec(nd).splitlines():
                f = ln.split()
                if len(f) < 9:
                    continue           # a line with no slots
                ep = f[1].split("@")[0]
                for rng in f[8:]:
                    a, _sep, b = rng.partition("-")
                    for s2 in range(int(a), int(b or a) + 1):
                        o3[s2] = ep
            d3 = sum(1 for s2 in range(16384)
                     if own.get(s2) is None or o3.get(s2) !=
                        "%s:%s" % (dec(own[s2][0]), own[s2][1]))
            print("NODESCMD %d %d" % (len(o3), d3))
    except Exception as e:
        import traceback
        at = traceback.format_exc().splitlines()[-2].strip()
        print("NODESCMD -1 -1 (%s: %s AT %s)" % (type(e).__name__, e, at))

elif what == "solo":
    r, _ = parse(call(host, port, [b"CLUSTER", b"KEYSLOT", b"foo"]))
    print("KEYSLOT %r" % (r,))
    r, _ = parse(call(host, port, [b"CLUSTER", b"SLOTS"]))
    print("SLOTS %r" % (r,))
EOF

fleet() { # fleet <count> - bring up N nodes and assert the whole surface
	#
	# Loop counters here are prefixed f_.  sh has no locals, and
	# start() spends `i` on its own wait loop: sharing the name meant
	# the first start() returned with i already past the node count,
	# so only node 1 was ever launched and every size reported "1
	# node".  The symptom looked exactly like a membership failure.
	N=$1
	f_n=1
	while [ $f_n -le "$N" ]; do node $f_n; f_n=$((f_n+1)); done
	f_n=1
	while [ $f_n -le "$N" ]; do start $f_n; f_n=$((f_n+1)); done

	echo "--- $N nodes ---"
	f_t=0; peers=0
	while [ $f_t -lt 60 ]; do
		peers=$(python3 "$PY" 127.0.0.71 17461 cover 2>"$D/cover.err" | \
			grep '^NODES ' | cut -d' ' -f2)
		[ "${peers:-0}" -ge "$N" ] && break
		sleep 0.5; f_t=$((f_t+1))
	done
	if [ "${peers:-0}" -lt "$N" ]; then
		bad "fleet never reached $N nodes in CLUSTER SLOTS (saw ${peers:-0});"
		echo "       nothing else for this size would mean anything"
		[ -s "$D/cover.err" ] && sed 's/^/       /' "$D/cover.err" | tail -4
		return 1
	fi
	ok "CLUSTER SLOTS shows all $N nodes"

	ks=$(python3 "$PY" 127.0.0.71 17461 keyslot)
	m=$(echo "$ks" | grep '^MISMATCHES ' | cut -d' ' -f2)
	if [ "${m:-99}" -eq 0 ]; then
		ok "KEYSLOT matches an independent CRC16 (tags included)"
	else
		bad "KEYSLOT disagrees with Redis's hash on $m key(s):"
		echo "$ks" | grep -v '^MISMATCHES' | sed 's/^/     /'
	fi

	cv=$(python3 "$PY" 127.0.0.71 17461 cover)
	once=$(echo "$cv" | grep '^ONCE ' | cut -d' ' -f2)
	gaps=$(echo "$cv" | grep '^ONCE ' | cut -d' ' -f4)
	ovl=$(echo "$cv"  | grep '^ONCE ' | cut -d' ' -f6)
	rng=$(echo "$cv"  | grep '^ONCE ' | cut -d' ' -f8)
	dis=$(echo "$cv"  | grep '^DISAGREE ' | cut -d' ' -f2)
	[ "${once:-0}" -eq 16384 ] \
		&& ok "SLOTS covers all 16384 slots exactly once ($rng ranges)" \
		|| bad "SLOTS covers $once/16384 once ($gaps gaps, $ovl overlapping)"
	if [ "${dis:-99}" -eq 0 ]; then
		ok "SHARDS describes the same assignment as SLOTS"
	elif [ "${dis:-99}" -eq -1 ]; then
		bad "CLUSTER SHARDS did not parse: $(echo "$cv" | grep '^DISAGREE ')"
	else
		bad "SLOTS and SHARDS disagree about $dis slot(s)"
	fi

	c1=$(python3 "$PY" 127.0.0.71 17461 cover | grep '^ONCE ')
	c2=$(python3 "$PY" 127.0.0.72 17462 cover | grep '^ONCE ')
	[ "$c1" = "$c2" ] && ok "peers publish the same slot map" \
		|| bad "node 1 and node 2 publish different maps: [$c1] vs [$c2]"

	# The advertised endpoints must be the RESP DOORS - the ports this
	# very test is dialling - never the native ones.  Shipped broken:
	# every reply carried client_port, whose door speaks only Noise
	# off-box, so a routing Redis client was told to dial a door that
	# cannot say hello.  Would have been caught here.
	ports=$(echo "$cv" | grep '^PORTS ' | cut -d' ' -f2)
	want=""
	f_p=1
	while [ $f_p -le "$N" ]; do
		want="$want${want:+,}127.0.0.7$f_p:1746$f_p"
		f_p=$((f_p+1))
	done
	[ "$ports" = "$want" ] && ok "SLOTS advertises the RESP doors" \
		|| bad "SLOTS advertises [$ports], want the RESP doors [$want]"

	nc=$(echo "$cv" | grep '^NODESCMD ')
	ncov=$(echo "$nc" | cut -d' ' -f2); ndis=$(echo "$nc" | cut -d' ' -f3)
	if [ "${ncov:-0}" = "16384" ] && [ "${ndis:-1}" = "0" ]; then
		ok "CLUSTER NODES covers 16384 and agrees with SLOTS"
	else
		bad "CLUSTER NODES: covered ${ncov:-?}, disagrees on ${ndis:-?} - $nc"
	fi

	f_n=1
	while [ $f_n -le "$N" ]; do
		eval "kill -9 \$P$f_n 2>/dev/null; P$f_n="
		f_n=$((f_n+1))
	done
	sleep 1
	return 0
}

# 4 is the size that used to fail: its reply passes 1MB, which the old
# fixed reply buffer refused.  2 is the smallest real cluster.
fleet 2
fleet 3
fleet 4

# ---- a node with no cluster -------------------------------------------
cat > "$D/solo.conf" <<EOF
[daemon]
workers = 1
log_level = info
[memory]
arena_mb = 32
[secrets]
client = cm-client-secret
# required even with no [cluster] section - the daemon refuses to start
# without one, so its absence would fail this node for the wrong reason
cluster = cm-cluster-secret
[listen]
tcp = 127.0.0.79:17459
resp = 127.0.0.79:17469
plaintext = loopback
[collection cm]
buckets_log2 = 10
EOF
"$BIN" -f "$D/solo.conf" > "$D/solo.log" 2>&1 &
P0=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/solo.log" && break
	sleep 0.1; i=$((i+1))
done
if ! grep -q "perfcached ready" "$D/solo.log"; then
	bad "the clusterless node did not start: $(tail -2 "$D/solo.log")"
	echo "clustermaptest: $pass passed, $fail failed"
	exit 1
fi
so=$(python3 "$PY" 127.0.0.79 17469 solo)
echo "$so" | grep -q "KEYSLOT 12182" \
	&& ok "KEYSLOT answers without a cluster (it is pure arithmetic)" \
	|| bad "clusterless KEYSLOT: $(echo "$so" | grep KEYSLOT)"
echo "$so" | grep -qi "SLOTS 'ERR.*cluster support disabled" \
	&& ok "SLOTS refuses without a cluster rather than inventing a map" \
	|| bad "clusterless SLOTS: $(echo "$so" | grep '^SLOTS')"

echo "clustermaptest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
