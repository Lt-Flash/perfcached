#!/bin/sh
# tlsdoortest.sh - the RESP and HTTP doors over TLS (OpenSSL memory BIOs at conn_take/conn_flush),
# under io_uring and epoll: handshakes, refusals, config checks, renewal by SIGHUP, /stats tls.
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pctls.XXXXXX)
NT=27211 RT=27212 RP=27213 HT=27214
PID=
trap '[ -n "$PID" ] && kill $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $NT $RT $RP $HT; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tlsdoortest: port $p already bound" >&2; exit 1; }
done
"$BIN" -V | grep -q '^tls: off' && { bad "this build has no TLS (PC_TLS=no) - the suite needs it"; exit 1; }
command -v openssl >/dev/null || { bad "no openssl CLI to make the test certificates"; exit 1; }
cd "$D" || exit 1
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj /CN=pctls-ca \
	-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign 2>/dev/null &&
openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr -subj /CN=localhost 2>/dev/null &&
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > ext.cnf &&
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out srv.pem -days 2 -extfile ext.cnf 2>/dev/null &&
openssl req -x509 -newkey rsa:2048 -nodes -keyout other.key -out other.pem -days 2 -subj /CN=other 2>/dev/null ||
	{ bad "the test certificates could not be made"; exit 1; }
conf() { # conf <name> <io> [extra [listen] lines...]
	n=$1 io=$2; shift 2
	mkdir -p "$D/s.$n" "$D/w.$n"
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
io = $io
state_dir = $D/s.$n
[memory]
arena_mb = 32
[secrets]
client = pctls-client
resp = pctls-resp-pw
http = pctls-http-token
[listen]
tcp = 127.0.0.1:$NT
plaintext = loopback
C
		for l in "$@"; do echo "$l"; done
		cat <<C
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/w.$n
probe = no
C
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
run_legs() { # run_legs <io>
	conf "$1" "$1" "resp = 127.0.0.1:$RT tls" "resp = 127.0.0.1:$RP" "http = 127.0.0.1:$HT tls" \
		"tls_cert = $D/srv.pem" "tls_key = $D/srv.key"
	: > "$D/$1.log"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log" || { bad "$1: the node did not start: $(tail -2 "$D/$1.log")"; exit 1; }
	r=$(python3 - $RT $RP $HT "$D" <<'PY'
import socket, ssl, sys, time, urllib.request
rt, rp, ht, d = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
def resp(s, *a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    f = s.makefile("rb"); l = f.readline()
    return f.read(int(l[1:]) + 2)[:-2] if l.startswith(b"$") and not l.startswith(b"$-1") else l.strip()
out = []
ctx = ssl.create_default_context(cafile=d + "/ca.pem")
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", rt), 5), server_hostname="127.0.0.1") as s:
    v = s.version()
    a = resp(s, b"AUTH", b"pctls-resp-pw"); st = resp(s, b"SET", b"big", b"v" * 40000); g = resp(s, b"GET", b"big")
    out.append("A=%s" % ("ok" if v in ("TLSv1.2", "TLSv1.3") and a == b"+OK" and st == b"+OK" and g == b"v" * 40000 else "%s/%r/%r/%d" % (v, a, st, len(g))))
    s.sendall(b"*1\r\n$4\r\nPING\r\n" * 50); buf = b""; t0 = time.time()
    while buf.count(b"+PONG") < 50 and time.time() - t0 < 5:
        buf += s.recv(65536)
    out.append("B=%d" % buf.count(b"+PONG"))
other = ssl.create_default_context(cafile=d + "/other.pem")
try:
    with other.wrap_socket(socket.create_connection(("127.0.0.1", rt), 5), server_hostname="127.0.0.1"):
        out.append("C=connected")
except ssl.SSLError:
    out.append("C=refused")
s = socket.create_connection(("127.0.0.1", rt), 5); s.settimeout(3); s.sendall(b"*1\r\n$4\r\nPING\r\n")
try:
    out.append("D=%r" % s.recv(100))
except Exception as e:
    out.append("D=%s" % type(e).__name__)
s = socket.create_connection(("127.0.0.1", rp), 5)
out.append("E=%s" % ("ok" if resp(s, b"AUTH", b"pctls-resp-pw") == b"+OK" and resp(s, b"GET", b"big") == b"v" * 40000 else "bad"))
try:
    r = urllib.request.urlopen(urllib.request.Request("https://127.0.0.1:%d/health" % ht,
        headers={"Authorization": "Bearer pctls-http-token"}), context=ctx, timeout=5)
    out.append("F=%d" % r.status)
except Exception as e:
    out.append("F=%s" % str(e)[:60])
s = socket.create_connection(("127.0.0.1", rt), 5); s.sendall(b"\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03"); s.close()
time.sleep(0.3)
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", rt), 5), server_hostname="127.0.0.1") as s:
    out.append("G=%s" % ("ok" if resp(s, b"AUTH", b"pctls-resp-pw") == b"+OK" and resp(s, b"PING") == b"+PONG" else "bad"))
# N: replies past the output pause while the client keeps sending - what the node reads meanwhile is
# records, held for after the pause, never parsed as commands
import threading
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", rt), 10), server_hostname="127.0.0.1") as s:
    resp(s, b"AUTH", b"pctls-resp-pw")
    val = lambda i: (b"%04d" % i) * 250
    req = b"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n" * 200 + b"".join(
        b"*3\r\n$3\r\nSET\r\n$5\r\nn%04d\r\n$1000\r\n%s\r\n*2\r\n$3\r\nGET\r\n$5\r\nn%04d\r\n" % (i, val(i), i)
        for i in range(50))
    th = threading.Thread(target=lambda: s.sendall(req)); th.start()
    time.sleep(1.0)                    # the replies pile up past the pause first
    f = s.makefile("rb"); bad_n = 0; got = 0
    try:
        for i in range(200):
            l = f.readline()
            bad_n += not (l.startswith(b"$40000") and f.read(40002)[:-2] == b"v" * 40000); got += 1
        for i in range(50):
            bad_n += f.readline() != b"+OK\r\n"
            l = f.readline()
            bad_n += not (l.startswith(b"$1000") and f.read(1002)[:-2] == val(i)); got += 1
        out.append("N=%s" % ("ok" if not bad_n else "%d-wrong" % bad_n))
    except Exception as e:
        out.append("N=lost-after-%d" % got)
    th.join(5)
# O: commands and the client's close_notify in one write - the commands still run, as on a plain door
sk = socket.create_connection(("127.0.0.1", rt), 5); bi, bo = ssl.MemoryBIO(), ssl.MemoryBIO()
ob = ctx.wrap_bio(bi, bo, server_hostname="127.0.0.1")
while True:
    try:
        ob.do_handshake(); break
    except ssl.SSLWantReadError:
        sk.sendall(bo.read()); bi.write(sk.recv(65536))
ob.write(b"*2\r\n$4\r\nAUTH\r\n$13\r\npctls-resp-pw\r\n*3\r\n$3\r\nSET\r\n$6\r\ntlseof\r\n$2\r\nok\r\n")
try:
    ob.unwrap()
except ssl.SSLWantReadError:
    pass
sk.sendall(bo.read()); time.sleep(0.5); sk.close()
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", rt), 5), server_hostname="127.0.0.1") as s:
    resp(s, b"AUTH", b"pctls-resp-pw"); g = resp(s, b"GET", b"tlseof")
    out.append("O=%s" % ("ok" if g == b"ok" else "lost-%r" % g))
print(" ".join(out))
PY
)
	set -- "$1" $r
	io=$1; shift
	for x in "$@"; do
		case "$x" in
		A=ok) ok "$io A. RESP over TLS: AUTH, a 40 KB value set and read back";;
		B=50) ok "$io B. 50 pipelined PINGs over TLS, 50 answers";;
		C=refused) grep -q "TLS handshake failed" "$D/$io.log" &&
			ok "$io C. a client trusting another CA refused, the node says why" ||
			bad "$io C. refused, but the node logged no handshake failure";;
		D=*) grep -q "wrong version number" "$D/$io.log" &&
			ok "$io D. a plaintext client on the TLS port closed ($x), the node says why" ||
			bad "$io D. plaintext on the TLS port: $x, nothing logged";;
		E=ok) ok "$io E. the plaintext RESP door beside it still plaintext";;
		N=ok) ok "$io N. 8 MB of replies past the output pause while the client kept sending: every reply right";;
		F=200) ok "$io F. GET /health over TLS with the token";;
		O=ok) ok "$io O. a SET and the client's close_notify in one write: the SET ran";;
		G=ok) grep -h 'an fsync has been stuck' "$D/$io.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
			n=$(grep -E "ERROR|CRIT" "$D/$io.log" | grep -cv 'an fsync has been stuck')
			[ "$n" = 0 ] && ok "$io G. a client gone mid-handshake: the node goes on, nothing in ERROR" ||
			bad "$io G. $n ERROR/CRIT line(s): $(grep -E 'ERROR|CRIT' "$D/$io.log" | grep -v 'an fsync has been stuck' | head -1)";;
		*) bad "$io $x";;
		esac
	done
	kill $PID; wait $PID 2>/dev/null; PID=
}
run_legs io_uring
run_legs epoll

conf h1 epoll "resp = 127.0.0.1:$RT tls"
"$BIN" -C -f "$D/h1.conf" > "$D/h1.out" 2>&1 && bad "H. a tls listener without tls_cert started" ||
	{ grep -q "a tls listener needs \[listen\] tls_cert and tls_key" "$D/h1.out" && ok "H. a tls listener without tls_cert refused, saying why" ||
		bad "H. refused without the reason: $(tail -1 "$D/h1.out")"; }
conf h2 epoll "resp = 127.0.0.1:$RT tls" "tls_cert = $D/srv.pem" "tls_key = $D/other.key"
"$BIN" -C -f "$D/h2.conf" > "$D/h2.out" 2>&1 && bad "H. a key that does not match the certificate started" ||
	{ grep -q "TLS: tls_key" "$D/h2.out" && ok "H. a key that does not match the certificate refused, saying why" ||
		bad "H. refused without the reason: $(tail -1 "$D/h2.out")"; }
conf h3 epoll "resp = 0.0.0.0:$RP" "resp_allow = 10.0.0.0/8"
"$BIN" -C -f "$D/h3.conf" > "$D/h3.out" 2>&1
grep -q "the RESP AUTH password travels in the clear" "$D/h3.out" &&
	ok "H. a plaintext RESP door off loopback with a password: warned about at -C" ||
	bad "H. no warning for the password in the clear: $(tail -2 "$D/h3.out")"
# a RESP or HTTP door off loopback with no password or token is warned about, not refused - a
# refusal would stop a live node at its upgrade
conf h4 epoll "resp = 0.0.0.0:$RP" "resp_allow = 10.0.0.0/8" "http = 0.0.0.0:$HT" "http_allow = 10.0.0.0/8"
sed -i '/^resp = pctls-resp-pw$/d; /^http = pctls-http-token$/d' "$D/h4.conf"
"$BIN" -C -f "$D/h4.conf" > "$D/h4.out" 2>&1; r=$?
[ $r = 0 ] && grep -q "a RESP listener off loopback has no password" "$D/h4.out" &&
	grep -q "an HTTP listener off loopback has no token" "$D/h4.out" &&
	ok "H. RESP and HTTP doors off loopback with no password or token: -C passes and warns about both" ||
	bad "H. no password, no token off loopback: -C exit $r, $(grep -iE 'password|token' "$D/h4.out" | head -2 | tr '\n' '|')"
# K, I, J: the 2-day certificate raises tls.cert; a 30-day renewal; /stats tls
conf ijk epoll "resp = 127.0.0.1:$RT tls" "http = 127.0.0.1:$HT" "tls_cert = $D/live.pem" "tls_key = $D/live.key"
cp srv.pem live.pem; cp srv.key live.key
: > "$D/ijk.log"
"$BIN" -f "$D/ijk.conf" > "$D/ijk.log" 2>&1 & PID=$!
i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/ijk.log"; do sleep 0.1; i=$((i+1)); done
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:'$HT'/stats", headers={"Authorization": "Bearer pctls-http-token"}), timeout=5).read())
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
peer() { python3 -c 'import hashlib,socket,ssl,sys
c=ssl.create_default_context(cafile=sys.argv[1])
s=c.wrap_socket(socket.create_connection(("127.0.0.1",int(sys.argv[2])),5),server_hostname="127.0.0.1")
print(hashlib.sha256(s.getpeercert(binary_form=True)).hexdigest()[:16])' "$D/ca.pem" $RT 2>&1 | tail -1; }
i=0; while [ $i -lt 50 ] && ! st '[a["key"] for a in s["alerts"]["active"]]' | grep -q tls.cert; do sleep 0.2; i=$((i+1)); done
a=$(st '[(a["key"], a["level"]) for a in s["alerts"]["active"] if a["key"] == "tls.cert"]')
case "$a" in *"'tls.cert', 'warning'"*) ok "K. a certificate inside 14 days raises tls.cert, a warning";; *) bad "K. tls.cert: '$a'";; esac
before=$(peer)
openssl req -newkey rsa:2048 -nodes -keyout new.key -out new.csr -subj /CN=localhost 2>/dev/null &&
openssl x509 -req -in new.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out new.pem -days 30 -extfile ext.cnf 2>/dev/null
cp new.pem live.pem; cp new.key live.key
kill -HUP $PID
i=0; while [ $i -lt 50 ] && ! st 's["reload"]["last"]' | grep -q "TLS certificate"; do sleep 0.1; i=$((i+1)); done
after=$(peer)
[ -n "$before" ] && [ ${#before} = 16 ] && [ "$before" != "$after" ] && [ ${#after} = 16 ] &&
	ok "I. a renewal (the files replaced, SIGHUP): the next connection gets the new certificate ($(st 's["reload"]["last"]'))" ||
	bad "I. renewal: before $before, after $after, reload '$(st 's["reload"]["last"]')'"
w=$(st '(lambda t: (t["days_left"] in (29, 30), t["handshakes"] >= 2, t["cert"]))(s["tls"])')
[ "$w" = "(True, True, '$D/live.pem')" ] && ok "J. /stats tls: the new certificate's days left, the handshakes" || bad "J. /stats tls: $w"
kill $PID; wait $PID 2>/dev/null; PID=

# L: auth_needs_tls, from 192.0.2.10 on a network namespace's lo - an address that is not loopback to the node
if ! unshare -n true 2>/dev/null; then
	[ -n "${GITLAB_CI:-}" ] && bad "L. no unshare -n on GitLab runner $(hostname): every GitLab runner must run it" ||
		echo "  ..   L. SKIPPED: no unshare -n here (not root?)"
else
	conf l epoll "resp = 0.0.0.0:$RT tls" "resp = 0.0.0.0:$RP" "http = 0.0.0.0:$HT" "resp_allow = 192.0.2.0/24, 127.0.0.0/8" \
		"http_allow = 192.0.2.0/24, 127.0.0.0/8" "tls_cert = $D/srv.pem" "tls_key = $D/srv.key" "auth_needs_tls = yes"
	cat > "$D/l.sh" <<'SH'
D=$1 BIN=$2 RT=$3 RP=$4 HT=$5 IP=192.0.2.10
ip link set lo up && ip addr add $IP/32 dev lo || { echo "1:no network in the namespace"; exit 0; }
: > "$D/l.log"
"$BIN" -f "$D/l.conf" > "$D/l.log" 2>&1 & PID=$!
i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/l.log"; do sleep 0.1; i=$((i+1)); done
cat > "$D/l.py" <<'PY'
import json, socket, ssl, sys, urllib.request, urllib.error
ip, rt, rp, ht, d, what = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6]
def resp(s, *a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    return s.makefile("rb").readline().strip().decode()
def plain(host):
    return resp(socket.create_connection((host, rp), 5), b"AUTH", b"pctls-resp-pw")
if what == "reload":
    s = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/stats" % ht,
        headers={"Authorization": "Bearer pctls-http-token"}), timeout=5).read())
    print(s["reload"]["last"])
elif what == "plain":
    print(plain(ip))
else:
    out = ["plain=" + plain(ip)[:40]]
    ctx = ssl.create_default_context(cafile=d + "/ca.pem")
    out.append("tls=" + resp(ctx.wrap_socket(socket.create_connection((ip, rt), 5), server_hostname="127.0.0.1"), b"AUTH", b"pctls-resp-pw"))
    out.append("loop=" + plain("127.0.0.1"))
    try:
        urllib.request.urlopen(urllib.request.Request("http://%s:%d/health" % (ip, ht), headers={"Authorization": "Bearer pctls-http-token"}), timeout=5)
        out.append("http=200")
    except urllib.error.HTTPError as e:
        out.append("http=%d" % e.code)
    print("|".join(out))
PY
l() { python3 "$D/l.py" $IP $RT $RP $HT "$D" "$1" 2>&1 | tail -1; }
echo "1:$(l all)"
sed -i '/^auth_needs_tls = yes$/d' "$D/l.conf"; kill -HUP $PID
i=0; while [ $i -lt 50 ] && ! l reload | grep -q "^reloaded"; do sleep 0.1; i=$((i+1)); done
echo "2:$(l plain)|$(l reload)"
kill $PID; wait $PID 2>/dev/null
SH
	unshare -n sh "$D/l.sh" "$D" "$BIN" $RT $RP $HT > "$D/l.out" 2>&1
	r=$(sed -n 's/^1://p' "$D/l.out")
	case "$r" in
	"plain=-ERR AUTH needs TLS here"*"|tls=+OK|loop=+OK|http=403")
		ok "L. auth_needs_tls: AUTH and the token refused in the clear from 192.0.2.10, taken over TLS and from loopback";;
	*) bad "L. auth_needs_tls: '$r' $(tail -1 "$D/l.log")";;
	esac
	r=$(sed -n 's/^2://p' "$D/l.out")
	case "$r" in
	"+OK|reloaded: "*auth_needs_tls*) ok "L. auth_needs_tls reloaded off: AUTH taken in the clear from 192.0.2.10 (${r#*|})";;
	*) bad "L. auth_needs_tls reload: '$r'";;
	esac
fi
PD="$(dirname "$BIN")/pagedump"
if [ -x "$PD" ]; then
	n=$("$PD" | grep -o -e "<div class=k>tls</div>" -e "certificate left" -e "tld<14" | sort -u | wc -l)
	[ "$n" = 3 ] && ok "M. the page has a tls card: the certificate's days left, amber inside 14" || bad "M. the page: $n of 3"
fi
echo "tlsdoortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
