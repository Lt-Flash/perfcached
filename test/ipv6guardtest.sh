#!/bin/sh
# ipv6guardtest.sh - 12np, IP6-2's first step: the IPv6 guard.  Before any
# door, tool or the library asks for IPv6, a FUNCTIONAL check says whether
# this host has it (pc_ipv6_usable: an AF_INET6 socket and a bind to
# [::1]:0), and every name the daemon resolves asks for IPv4 only when it
# has not - no AAAA query goes out from an IPv6-less host (the operator,
# 2026-10-21).
#
# Each half runs in its own network and mount namespace (unshare -n -m):
# a stub resolver on 127.0.0.1:53 logs every query's type, the namespace's
# /etc/resolv.conf and /etc/nsswitch.conf point at it ("hosts: files dns"),
# and nscd's socket is hidden (a host's nscd would answer from outside).
# The node names its native listener and its upstream "pcv6guard.test".
#  off: disable_ipv6 = 1 in the namespace - -C says "net: IPv6 is not
#       usable ... no ::1 to bind", the start log says it once, the node
#       resolves both names with A queries and NO AAAA, a read through the
#       named upstream answers, and an EXPLICIT IPv6 listener ([::1]:port)
#       is refused at -C by name, with the reason (IP6-2)
#  on:  IPv6 as the host has it - -C says "usable", the same start sends
#       AAAA queries too (the count can see one: the instrument can fail),
#       and the [::1] listener passes -C
# Needs root (unshare -n -m, mount, the namespace's sysctl): skipped loudly
# where that is not possible, and FAILED under GitLab CI, where it must be.
# Usage: test/ipv6guardtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcv6g.XXXXXX)
trap '[ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skip() {
	if [ -n "${GITLAB_CI:-}" ]; then
		echo "ipv6guardtest: FAILED - $1 on GitLab runner $(hostname): every GitLab runner must run it"
		exit 1
	fi
	echo "ipv6guardtest: SKIP - $1 - nothing was tested"
	exit 0
}
unshare -n -m true 2>/dev/null || skip "no unshare -n -m here (not root?)"

# the stub resolver: A for pcv6guard.test is 127.0.0.1, AAAA has no
# record (NODATA, so no client binds or dials ::1), any other name NXDOMAIN
cat > "$D/stubdns.py" <<'PY'
import socket, struct, sys
host, port, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((host, port))
open(log + ".ready", "w").close()
while True:
    q, a = s.recvfrom(512)
    if len(q) < 17:
        continue
    tid = struct.unpack(">H", q[:2])[0]
    i, labels = 12, []
    while i < len(q) and q[i]:
        n = q[i]
        labels.append(q[i + 1:i + 1 + n].decode("ascii", "replace"))
        i += 1 + n
    i += 1
    if i + 4 > len(q):
        continue
    qtype = struct.unpack(">H", q[i:i + 2])[0]
    name = ".".join(labels).lower()
    with open(log, "a") as f:
        f.write("%s %d\n" % (name, qtype))
    ans, an = b"", 0
    if name == "pcv6guard.test" and qtype == 1:
        ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 0, 4) + socket.inet_aton("127.0.0.1")
        an = 1
    rcode = 0 if name == "pcv6guard.test" else 3
    s.sendto(struct.pack(">HHHHHH", tid, 0x8180 | rcode, 1, an, 0, 0) + q[12:i + 4] + ans, a)
PY

# the node: its native listener and its upstream by name
cat > "$D/n.conf" <<C
[daemon]
workers = 1
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = v6guard-client-secret
[listen]
tcp = pcv6guard.test:17954
resp = 127.0.0.1:17952
http = 127.0.0.1:17953
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://pcv6guard.test:17951
upstream_prefixes = *
upstream_timeout_ms = 1000
C
chmod 600 "$D/n.conf"
# the same with an explicit IPv6 listener beside
sed 's/^resp = 127.0.0.1:17952$/resp = 127.0.0.1:17952\nresp = [::1]:17955/' "$D/n.conf" > "$D/n6.conf"
chmod 600 "$D/n6.conf"

# inside the namespace: the resolver, the fake upstream, -C, the node, a read
cat > "$D/inner.sh" <<'SH'
set -u
mode=$1 D=$2 BIN=$3 T=$4
ip link set lo up || exit 3
if [ "$mode" = off ]; then
	for f in all default lo; do
		echo 1 > /proc/sys/net/ipv6/conf/$f/disable_ipv6 || exit 3
	done
fi
printf 'nameserver 127.0.0.1\noptions attempts:2 timeout:1\n' > "$D/resolv.conf"
printf 'hosts: files dns\n' > "$D/nsswitch.conf"
mkdir -p "$D/nonscd"
mount --bind "$D/resolv.conf" /etc/resolv.conf || exit 4
mount --bind "$D/nsswitch.conf" /etc/nsswitch.conf || exit 4
for n in /run/nscd /var/run/nscd; do
	[ -d "$n" ] && { mount --bind "$D/nonscd" "$n" || exit 4; }
done
python3 "$D/stubdns.py" 127.0.0.1 53 "$D/q.$mode" > "$D/dns.$mode.log" 2>&1 & DNS=$!
mkdir -p "$D/up.$mode"
echo '{"g:1": {"type": "string", "value": "v1"}}' > "$D/up.$mode/data.json"
echo '{}' > "$D/up.$mode/ctl.json"
python3 "$T/fakeupstream.py" 17951 "$D/up.$mode" > "$D/f.$mode.log" 2>&1 & F=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/q.$mode.ready" ] && [ -f "$D/up.$mode/ready" ]; }; do
	sleep 0.1; i=$((i+1)); done
"$BIN" -C -f "$D/n.conf" > "$D/c.$mode.out" 2>&1
"$BIN" -C -f "$D/n6.conf" > "$D/c6.$mode.out" 2>&1; echo $? > "$D/c6.$mode.rc"
rm -rf "$D/state"; mkdir -p "$D/state"
"$BIN" -f "$D/n.conf" > "$D/n.$mode.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.$mode.log"; do
	kill -0 $P 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
python3 - > "$D/get.$mode" 2>&1 <<'PY'
import socket
k = socket.create_connection(("127.0.0.1", 17952), 5); k.settimeout(5)
k.sendall(b"*2\r\n$3\r\nGET\r\n$3\r\ng:1\r\n")
f = k.makefile("rb"); l = f.readline()
print(f.read(int(l[1:-2]) + 2)[:-2].decode() if l[:1] == b"$" and int(l[1:-2]) >= 0 else l.decode().strip())
PY
kill $P 2>/dev/null; wait $P 2>/dev/null
kill $F $DNS 2>/dev/null; wait $F $DNS 2>/dev/null
exit 0
SH

for mode in off on; do
	unshare -n -m sh "$D/inner.sh" $mode "$D" "$BIN" "$T"
	rc=$?
	[ $rc = 3 ] && skip "cannot set up the namespace's network or disable IPv6 in it"
	[ $rc = 4 ] && skip "cannot bind-mount resolv.conf / nsswitch.conf in a private mount namespace"
	[ -f "$D/q.$mode.ready" ] || skip "the stub resolver did not start ($(tail -2 "$D/dns.$mode.log" 2>/dev/null))"
	a=$(grep -c '^pcv6guard\.test 1$' "$D/q.$mode" 2>/dev/null); a=${a:-0}
	aaaa=$(grep -c '^pcv6guard\.test 28$' "$D/q.$mode" 2>/dev/null); aaaa=${aaaa:-0}
	g=$(cat "$D/get.$mode" 2>/dev/null)
	if [ "$mode" = off ]; then
		grep -q '^net: IPv6 is not usable on this host - no ::1 to bind' "$D/c.$mode.out" &&
			ok "off: -C says IPv6 is not usable, and why ($(grep '^net: ' "$D/c.$mode.out" | cut -c1-90))" ||
			bad "off: -C: '$(grep '^net: ' "$D/c.$mode.out")'"
		grep -q 'net: IPv6 is not usable on this host' "$D/n.$mode.log" &&
			ok "off: the start log says it once" ||
			bad "off: no 'IPv6 is not usable' line in the start log"
		[ "$a" -ge 2 ] && [ "$aaaa" = 0 ] &&
			ok "off: the named listener and upstream resolved with $a A queries and no AAAA" ||
			bad "off: $a A, $aaaa AAAA queries for pcv6guard.test (expected >= 2 and 0)"
		[ "$g" = v1 ] && ok "off: a read through the named upstream answers ('$g')" ||
			{ bad "off: the read answered '$g'"; tail -5 "$D/n.$mode.log"; }
		[ "$(cat "$D/c6.$mode.rc")" != 0 ] &&
		    grep -q 'listener \[::1\]:17955 is IPv6, but IPv6 is not usable on this host - no ::1 to bind' "$D/c6.$mode.out" &&
			ok "off: an explicit [::1] listener is refused at -C, named, with the reason" ||
			bad "off: the [::1] listener at -C: rc $(cat "$D/c6.$mode.rc"), '$(grep -i 'listener\|ipv6' "$D/c6.$mode.out" | head -1)'"
	else
		grep -q '^net: IPv6 is usable on this host$' "$D/c.$mode.out" &&
			ok "on: -C says IPv6 is usable" ||
			bad "on: -C: '$(grep '^net: ' "$D/c.$mode.out")'"
		[ "$aaaa" -ge 1 ] &&
			ok "on: the same start sends AAAA queries too ($a A, $aaaa AAAA) - the count can see one" ||
			bad "on: $a A, $aaaa AAAA - no AAAA with IPv6 usable: this rig cannot show the defect"
		[ "$g" = v1 ] && ok "on: a read through the named upstream answers ('$g')" ||
			{ bad "on: the read answered '$g'"; tail -5 "$D/n.$mode.log"; }
		[ "$(cat "$D/c6.$mode.rc")" = 0 ] && ok "on: the [::1] listener passes -C" ||
			bad "on: the [::1] listener at -C: rc $(cat "$D/c6.$mode.rc"), '$(grep -iE 'error|refus|listener' "$D/c6.$mode.out" | head -1)'"
	fi
done
echo "ipv6guardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
