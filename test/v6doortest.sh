#!/bin/sh
# v6doortest.sh - 12np IP6-2: the client doors on IPv6.  The fleet stays
# IPv4 (IP6-3 is the cluster's); clients may come over either family.
#  A. a node whose native, RESP and http doors listen on [::1] only: -C
#     takes them and says IPv6 is usable; perfcli, a RESP client and an
#     http client reach it over ::1; 127.0.0.1 does not
#  B. dual-stack doors ([::]:port, IPV6_V6ONLY off): an IPv4 client
#     arrives as ::ffff:127.0.0.1 and is matched against an IPv4
#     resp_allow as the IPv4 address it is; ::1 is refused by that list
#     (and the refusal names ::1); http_allow = ::1 lets ::1 in and
#     keeps 127.0.0.1 out; resp_allow with "[::1]/128" lets ::1 in; a
#     prefix past 128 and a non-address are refused at -C
#  C. an upstream at redis://[::1]:port: the read fetches through it, and
#     /stats names it in brackets
#  D. a client on ::1 writes through one member's [::1] RESP door and
#     reads the key on another member's - the fleet formed on 127.0.88.x
# Needs IPv6 on this host (a bind to [::1]): a loud SKIP without it, and
# FAILED under GitLab CI, where every runner has it.
# Usage: test/v6doortest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcv6d.XXXXXX)
PA= PB= PC= PF= P1= P2= PID=
trap 'for p in $PA $PB $PC $PF $P1 $P2 $PID; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

v6=$(python3 -c 'import socket
s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM); s.bind(("::1", 0)); print("yes")' 2>/dev/null)
if [ "$v6" != yes ]; then
	if [ -n "${GITLAB_CI:-}" ]; then
		echo "v6doortest: FAILED - IPv6 is not usable on GitLab runner $(hostname) (no ::1 to bind): every GitLab runner must run it"
		exit 1
	fi
	echo "v6doortest: SKIP - IPv6 is not usable here (no ::1 to bind) - nothing was tested"
	exit 0
fi
for p in 16201 16202 16203 16211 16212 16213 16221 16222 16223 16224 16231 16232 16241 16242 16251 16252 16261 16262 16290; do
	ss -ltnu 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "v6doortest: port $p already bound" >&2; exit 1; }
done

# a client of either family: RESP (one command, "closed" when the door
# hangs up before answering) and http (/stats and an expression over it)
cat > "$D/cl.py" <<'PY'
import json, socket, sys, urllib.request
mode, h, p = sys.argv[1], sys.argv[2], int(sys.argv[3])
if mode == "resp":
    args = [a.encode() for a in sys.argv[4:]]
    try:
        s = socket.socket(socket.AF_INET6 if ":" in h else socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((h, p))
        f = s.makefile("rb")
        s.sendall(b"*%d\r\n" % len(args) + b"".join(b"$%d\r\n%s\r\n" % (len(a), a) for a in args))
        l = f.readline()
    except OSError:
        l = b""
    if not l:
        print("closed")
    elif l[:1] == b"$":
        n = int(l[1:-2])
        print("nil" if n < 0 else f.read(n + 2)[:-2].decode())
    else:
        print(l[1:-2].decode() if l[:1] in (b"+", b":") else l.decode().strip())
else:
    url = "http://%s:%d%s" % ("[%s]" % h if ":" in h else h, p, sys.argv[4])
    try:
        s = json.load(urllib.request.urlopen(url, timeout=5))
    except Exception:
        print("closed")
        sys.exit(0)
    print(eval(sys.argv[5]))
PY
cl() { python3 "$D/cl.py" "$@" 2>&1; }
waitfor() { i=0; while [ $i -lt $(( $1 * 5 )) ] && ! eval "$2"; do sleep 0.2; i=$((i+1)); done; eval "$2"; }
head_conf() { # head_conf <name> - [daemon] [memory] [secrets] (the section left open)
	cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s.$1
[memory]
arena_mb = 64
[secrets]
client = v6d-client-secret
C
}
start() { # start <name> - the node from $D/<name>.conf, its pid in $PID
	rm -rf "$D/s.$1"; mkdir -p "$D/s.$1"; chmod 600 "$D/$1.conf"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log" && return 0
	bad "$1 did not start: $(tail -3 "$D/$1.log" | tr '\n' ' ' | cut -c1-300)"
	PID=
	return 1
}
stop() { [ -n "$1" ] && { kill $1 2>/dev/null; wait $1 2>/dev/null; }; }

# ---- A. the doors on [::1] only ----------------------------------------------
{ head_conf a; cat <<C
[listen]
tcp = [::1]:16201
resp = [::1]:16202
http = [::1]:16203
plaintext = loopback
[collection 0]
buckets_log2 = 10
[collection c]
buckets_log2 = 10
C
} > "$D/a.conf"
chmod 600 "$D/a.conf"
"$BIN" -C -f "$D/a.conf" > "$D/a.C" 2>&1; rc=$?
[ $rc = 0 ] && grep -q '^net: IPv6 is usable on this host$' "$D/a.C" &&
	ok "A. -C takes [::1] listeners and says IPv6 is usable here" ||
	bad "A. -C rc $rc: $(grep -iE 'net:|error|refus' "$D/a.C" | head -2)"
if start a; then
	PA=$PID
	# plaintext: a loopback door under plaintext = loopback carries no PSK
	# (a Noise handshake there is refused, on 127.0.0.1 as on ::1)
	r=$("$CLI" -q -h ::1 -p 16201 set c k1 v1 2>&1)
	g=$("$CLI" -q -h ::1 -p 16201 get c k1 2>&1)
	case "$g" in *v1*) ok "A. the native door on [::1]: perfcli set, then get '$g'";;
	*) bad "A. perfcli on ::1: set '$r', get '$g'";; esac
	cl resp ::1 16202 SET r1 x > /dev/null
	g=$(cl resp ::1 16202 GET r1)
	[ "$g" = x ] && ok "A. the RESP door on [::1]: SET, then GET '$g'" || bad "A. RESP on ::1: '$g'"
	u=$(cl http ::1 16203 /stats 's["io"]["ipv6"]["usable"]')
	[ "$u" = True ] && ok "A. the http door on [::1]: /stats io.ipv6.usable $u" || bad "A. http on ::1: '$u'"
	g=$(cl resp 127.0.0.1 16202 PING)
	[ "$g" = closed ] && ok "A. a [::1] door is IPv6 only: 127.0.0.1 does not reach it" ||
		bad "A. 127.0.0.1:16202 answered '$g'"
	stop $PA; PA=
fi

# ---- B. dual-stack doors and the allow lists -----------------------------------
bconf() { # bconf <name> <resp_allow> <http_allow>
	{ head_conf $1; cat <<C
[listen]
tcp = 127.0.0.1:16211
resp = [::]:16212
http = [::]:16213
plaintext = loopback
resp_allow = $2
http_allow = $3
[collection 0]
buckets_log2 = 10
C
	} > "$D/$1.conf"
}
bconf b "127.0.0.0/8" "::1"
if start b; then
	PB=$PID
	g=$(cl resp 127.0.0.1 16212 PING)
	[ "$g" = PONG ] && ok "B. an IPv4 client of [::]:port (::ffff:127.0.0.1) is in resp_allow = 127.0.0.0/8: $g" ||
		bad "B. 127.0.0.1 on the dual-stack RESP door: '$g'"
	g=$(cl resp ::1 16212 PING)
	[ "$g" = closed ] && ok "B. ::1 is not in resp_allow = 127.0.0.0/8: refused" || bad "B. ::1 answered '$g'"
	# the address alone, as an IPv4 client's is (pc_addr_str)
	waitfor 3 'grep -q "RESP connection from ::1 refused" "$D/b.log"' &&
		ok "B. the refusal names the client: $(grep -o 'RESP connection from ::1 refused' "$D/b.log" | head -1)" ||
		{ bad "B. no refusal line naming [::1] in the log"
		  grep -iE 'refus|resp_allow|RESP listener|listen' "$D/b.log" | tail -6 | cut -c1-200 | sed 's/^/      log: /'; }
	g=$(cl http ::1 16213 /stats '"in"')
	[ "$g" = in ] && ok "B. http_allow = ::1 lets ::1 in" || bad "B. ::1 on the http door: '$g'"
	g=$(cl http 127.0.0.1 16213 /stats '"in"')
	[ "$g" = closed ] && ok "B. 127.0.0.1 is not in http_allow = ::1: refused" || bad "B. 127.0.0.1 on the http door: '$g'"
	stop $PB; PB=
fi
bconf b2 "127.0.0.0/8, [::1]/128" "::1"
if start b2; then
	PB=$PID
	g=$(cl resp ::1 16212 PING)
	[ "$g" = PONG ] && ok "B. resp_allow = 127.0.0.0/8, [::1]/128 lets ::1 in: $g" || bad "B. ::1 with [::1]/128 listed: '$g'"
	stop $PB; PB=
fi
bconf b3 "::1/129" "::1"
"$BIN" -C -f "$D/b3.conf" > "$D/b3.C" 2>&1; rc=$?
[ $rc != 0 ] && ok "B. resp_allow = ::1/129 is refused at -C (rc $rc)" || bad "B. ::1/129 passed -C"
bconf b4 "fe80::zz" "::1"
"$BIN" -C -f "$D/b4.conf" > "$D/b4.C" 2>&1; rc=$?
[ $rc != 0 ] && grep -q "is not an IPv4 or IPv6 address or CIDR" "$D/b4.C" &&
	ok "B. resp_allow = fe80::zz is refused at -C, named" || bad "B. fe80::zz: rc $rc, '$(grep -i allow "$D/b4.C" | head -1)'"

# ---- C. an upstream on [::1] ---------------------------------------------------
mkdir -p "$D/up"
echo '{"u:1": {"type": "string", "value": "uv"}}' > "$D/up/data.json"
echo '{}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" 16221 "$D/up" ::1 > "$D/f.log" 2>&1 & PF=$!
i=0; while [ $i -lt 50 ] && [ ! -f "$D/up/ready" ]; do sleep 0.1; i=$((i+1)); done
{ head_conf c; cat <<C
[listen]
tcp = 127.0.0.1:16224
resp = 127.0.0.1:16222
http = 127.0.0.1:16223
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://[::1]:16221
upstream_prefixes = *
upstream_timeout_ms = 1000
C
} > "$D/c.conf"
if start c; then
	PC=$PID
	g=$(cl resp 127.0.0.1 16222 GET u:1)
	[ "$g" = uv ] && ok "C. upstream = redis://[::1]:16221: the read fetched '$g'" || bad "C. the read answered '$g'"
	a=$(cl http 127.0.0.1 16223 /stats '",".join(x["addr"] for x in [c for c in s["collections"] if c["name"] == "0"][0]["upstream"]["upstreams"])')
	[ "$a" = "[::1]:16221" ] && ok "C. /stats names the upstream in brackets: $a" || bad "C. the upstream's addr: '$a'"
	stop $PC; PC=
fi
stop $PF; PF=

# ---- D. a client on ::1, the fleet on 127.0.88.x -------------------------------
mconf() { # mconf <n>
	{ head_conf m$1; cat <<C
cluster = v6d-cluster-secret
[listen]
tcp = 127.0.88.$1:1623$1
tcp = [::1]:1624$1
resp = [::1]:1625$1
http = 127.0.88.$1:1626$1
plaintext = loopback
[cluster]
multicast = 239.255.88.33:16290
advertise = 127.0.88.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
C
	} > "$D/m$1.conf"
}
mconf 1; mconf 2
if start m1; then
	P1=$PID
	if start m2; then
		P2=$PID
		waitfor 30 '[ "$(cl http 127.0.88.1 16261 /stats "s[\"cluster\"][\"peers_up\"]")" = 1 ] && [ "$(cl http 127.0.88.2 16262 /stats "s[\"cluster\"][\"peers_up\"]")" = 1 ] && [ "$(cl http 127.0.88.1 16261 /stats "s[\"state\"]")" = ready ] && [ "$(cl http 127.0.88.2 16262 /stats "s[\"state\"]")" = ready ]' &&
			ok "D. two members up and ready on 127.0.88.x" ||
			bad "D. peers_up $(cl http 127.0.88.1 16261 /stats 's["cluster"]["peers_up"]') / $(cl http 127.0.88.2 16262 /stats 's["cluster"]["peers_up"]')"
		w=$(cl resp ::1 16251 SET d:1 dv)
		waitfor 10 '[ "$(cl resp ::1 16252 GET d:1)" = dv ]' &&
			ok "D. a client on ::1 wrote through member 1 ($w) and read it on member 2: $(cl resp ::1 16252 GET d:1)" ||
			bad "D. member 2 over ::1 answered '$(cl resp ::1 16252 GET d:1)' (the SET: '$w')"
		g=$("$CLI" -q -h ::1 -p 16242 get 0 d:1 2>&1)
		case "$g" in *dv*) ok "D. perfcli on member 2's [::1] native door reads it too: '$g'";;
		*) bad "D. perfcli on ::1:16242: '$g'";; esac
		stop $P2; P2=
	fi
	stop $P1; P1=
fi
echo "v6doortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
