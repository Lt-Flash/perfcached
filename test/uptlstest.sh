#!/bin/sh
# uptlstest.sh - rediss:// upstreams against TLS fakes from a throwaway CA: verification, names,
# client certificates, sentinel, config refusals and reload.  Usage: test/uptlstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcuptls.XXXXXX)
PU=27221 PN=27222 PV=27223 PC=27224 PS=27225 TCP=27226 RESP=27227 HTTP=27228
PID= FU= FN= FV= FC= FS=
trap 'kill $PID 2>/dev/null; for p in $FU $FN $FV $FC $FS; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PU $PN $PV $PC $PS $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "uptlstest: port $p already bound" >&2; exit 1; }
done
"$BIN" -V | grep -q '^tls: off' && { bad "this build has no TLS (PC_TLS=no) - the suite needs it"; exit 1; }
cd "$D" || exit 1
ca() { # ca <name>
	openssl req -x509 -newkey rsa:2048 -nodes -keyout $1.key -out $1.pem -days 2 -subj /CN=$1 \
		-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign 2>/dev/null
}
leaf() { # leaf <name> <CN> <SAN> <serverAuth|clientAuth>
	printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=%s\n' \
		"$3" "$4" > $1.cnf &&
	openssl req -newkey rsa:2048 -nodes -keyout $1.key -out $1.csr -subj /CN=$2 2>/dev/null &&
	openssl x509 -req -in $1.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out $1.pem -days 2 -extfile $1.cnf 2>/dev/null
}
ca ca && ca other && leaf srv localhost IP:127.0.0.1 serverAuth && leaf named redis.test DNS:redis.test serverAuth &&
leaf client pc-client DNS:pc-client clientAuth &&
openssl req -x509 -newkey rsa:2048 -nodes -keyout self.key -out self.pem -days 2 -subj /CN=self \
	-addext subjectAltName=IP:127.0.0.1 2>/dev/null || { bad "the test certificates could not be made"; exit 1; }
fake() { # fake <dir> <port> <FAKEUP_TLS> <ctl json>
	rm -rf "$D/$1"; mkdir -p "$D/$1"
	python3 -c 'import json,sys; json.dump({"k%d" % i: {"type": "string", "value": sys.argv[1]} for i in range(10)}, open(sys.argv[2], "w"))' "$1" "$D/$1/data.json"
	echo "$4" > "$D/$1/ctl.json"
	FAKEUP_TLS=$3 python3 "$T/fakeupstream.py" $2 "$D/$1" > "$D/$1.log" 2>&1 &
}
fake u $PU "$D/srv.pem:$D/srv.key" '{"role": "master", "accept_writes": true}'; FU=$!
fake n $PN "$D/named.pem:$D/named.key" '{"role": "master"}'; FN=$!
fake v $PV "$D/self.pem:$D/self.key" '{"role": "master"}'; FV=$!
fake c $PC "$D/srv.pem:$D/srv.key:$D/ca.pem" '{"role": "master"}'; FC=$!
fake s $PS "$D/srv.pem:$D/srv.key" '{"redis_mode": "sentinel", "sentinel": {"mym": {"ip": "127.0.0.1", "port": '$PU', "replicas": [], "sentinels": []}}}'; FS=$!
i=0; while [ $i -lt 50 ] && ! { [ -f u/ready ] && [ -f n/ready ] && [ -f v/ready ] && [ -f c/ready ] && [ -f s/ready ]; }; do sleep 0.1; i=$((i+1)); done
conf() { # conf <name> <upstream url> [extra collection lines...]
	n=$1 u=$2; shift 2
	rm -rf "$D/state"; mkdir -p "$D/state"
	{
		cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = uptls-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 8
upstream = $u
upstream_prefixes = *
upstream_timeout_ms = 2000
upstream_negative_ms = 500
C
		for l in "$@"; do echo "$l"; done
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
start() { # start <name> <upstream url> [extra collection lines...]
	[ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }     # a bare wait waits for the fakes too
	PID=
	conf "$@"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log" || { bad "the node ($1) did not start: $(tail -2 "$D/$1.log")"; return 1; }
}
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
c=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
reads() { python3 - $RESP <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
got = {}
for i in range(10):
    k = b"k%d" % i
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(k), k))
    l = f.readline()
    v = f.read(int(l[1:]) + 2)[:-2].decode() if l.startswith(b"$") and not l.startswith(b"$-1") else l.decode().strip()[:40]
    got[v] = got.get(v, 0) + 1
print(" ".join("%s=%d" % kv for kv in sorted(got.items())))
PY
}
cmd() { python3 - $RESP "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10); f = s.makefile("rb")
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
l = f.readline()
print(f.read(int(l[1:]) + 2)[:-2].decode() if l.startswith(b"$") and not l.startswith(b"$-1") else l.decode().strip()[:80])
PY
}

# A-C: the CA named, reads, an invalidation, a write through
if start a "rediss://127.0.0.1:$PU" "upstream_tls_ca = $D/ca.pem" "upstream_writes = through"; then
	r=$(reads)
	w=$(st '(lambda t: (t["verify"], t["handshakes"] >= 1, t["failed"]))(c["tls"])')
	[ "$r" = "u=10" ] && [ "$w" = "(True, True, 0)" ] && grep -q '^ok -' u/tls &&
		ok "A. rediss:// with upstream_tls_ca: reads over TLS ($r), handshakes counted, none failed" ||
		bad "A. reads '$r', /stats tls '$w', the fake's handshakes: $(head -2 u/tls 2>/dev/null | tr '\n' ' ')"
	python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d["k0"]["value"]="u2"; json.dump(d, open(sys.argv[1], "w"))' u/data.json
	echo '{"role": "master", "accept_writes": true, "inv_seq": 1, "invalidate": ["k0"]}' > u/ctl.json
	i=0; while [ $i -lt 25 ] && [ "$(cmd GET k0)" != "u2" ]; do sleep 0.2; i=$((i+1)); done
	g=$(cmd GET k0)
	[ "$g" = "u2" ] && ok "B. an invalidation pushed over TLS dropped the copy: GET k0 = u2" ||
		bad "B. after the invalidation GET k0 = '$g'"
	cmd SET k50 tlsw > /dev/null
	i=0; while [ $i -lt 25 ] && ! grep -q '^SET k50 tlsw' u/writes 2>/dev/null; do sleep 0.2; i=$((i+1)); done
	grep -q '^SET k50 tlsw' u/writes 2>/dev/null && ok "C. a write through reached the master over TLS" ||
		bad "C. the write did not reach the master: $(tail -1 u/writes 2>/dev/null)"
fi

# D: the wrong CA
if start d "rediss://127.0.0.1:$PU" "upstream_tls_ca = $D/other.pem"; then
	r=$(reads); sleep 0.5
	w=$(st '(lambda t: (t["failed"] >= 1, t["last_failure"]))(c["tls"])')
	case "$r|$w" in
	*u=*) bad "D. read through a certificate the CA did not sign: $r";;
	*"|(True, '"*certificate*) grep -q "TLS handshake failed" d.log &&
		ok "D. a CA that did not sign the upstream's certificate: refused, said why ($w)" ||
		bad "D. refused, but the log says nothing: $(grep -i tls d.log | tail -1)";;
	*) bad "D. reads '$r', /stats tls '$w'";;
	esac
fi

# E: the name
if start e1 "rediss://127.0.0.1:$PN" "upstream_tls_ca = $D/ca.pem"; then
	r1=$(reads)
	start e2 "rediss://127.0.0.1:$PN" "upstream_tls_ca = $D/ca.pem" "upstream_tls_name = redis.test" && r2=$(reads)
	case "$r1|${r2:-}" in
	*n=*"|n=10") bad "E. dialled as 127.0.0.1, a certificate naming redis.test was taken: $r1";;
	*"|n=10") ok "E. a certificate naming redis.test refused as 127.0.0.1, taken with upstream_tls_name = redis.test";;
	*) bad "E. as 127.0.0.1 '$r1', with upstream_tls_name '${r2:-}'";;
	esac
fi

# F: verify off
conf f "rediss://127.0.0.1:$PV" "upstream_tls_verify = no"
"$BIN" -C -f "$D/f.conf" > f.check 2>&1
if start f "rediss://127.0.0.1:$PV" "upstream_tls_verify = no"; then
	r=$(reads)
	[ "$r" = "v=10" ] && grep -q "upstream_tls_verify = no" f.check &&
		ok "F. a self-signed certificate, upstream_tls_verify = no: reads ($r), -C warns it is not checked" ||
		bad "F. reads '$r', -C: $(grep -i 'tls' f.check | head -1)"
fi

# G: a client certificate
if start g1 "rediss://127.0.0.1:$PC" "upstream_tls_ca = $D/ca.pem"; then
	r1=$(reads)
	start g2 "rediss://127.0.0.1:$PC" "upstream_tls_ca = $D/ca.pem" "upstream_tls_cert = $D/client.pem" \
		"upstream_tls_key = $D/client.key" && r2=$(reads)
	case "$r1|${r2:-}" in
	*c=*"|"*) bad "G. read without the client certificate the upstream requires: $r1";;
	*"|c=10") grep -q '^ok pc-client' c/tls &&
		ok "G. refused without a client certificate, read with upstream_tls_cert as pc-client" ||
		bad "G. read, but the fake did not see pc-client: $(tail -1 c/tls)";;
	*) bad "G. without '$r1', with '${r2:-}'";;
	esac
fi

# H: a sentinel over TLS, naming the master U
if start h "rediss://127.0.0.1:$PS" "upstream_tls_ca = $D/ca.pem"; then
	i=0; while [ $i -lt 50 ] && [ "$(st '[x["addr"].rsplit(":",1)[1] for x in c["upstreams"] if x["origin"] == "sentinel"]')" != "['$PU']" ]; do
		sleep 0.2; i=$((i+1)); done
	r=$(reads)
	case "$r" in
	*u=*) ok "H. a sentinel over TLS: the master it names added, read over TLS ($r)";;
	*) bad "H. sentinel over TLS: added $(st '[x["addr"] for x in c["upstreams"]]'), reads '$r', $(st 'c.get("sentinel")')";;
	esac
fi

# J: a reload reads the files again - the wrong CA first, the right one put in its place, SIGHUP
cp other.pem cur-ca.pem
if start j "rediss://127.0.0.1:$PU" "upstream_tls_ca = $D/cur-ca.pem"; then
	r1=$(reads)
	cp ca.pem cur-ca.pem
	kill -HUP $PID
	i=0; while [ $i -lt 50 ] && ! grep -q "reload" j.log; do sleep 0.1; i=$((i+1)); done
	sleep 0.6                      # past the negative marks of the failed reads
	r2=$(reads)
	echo "not a certificate" > cur-ca.pem
	kill -HUP $PID
	i=0; while [ $i -lt 50 ] && [ "$(grep -c reload j.log)" -lt 2 ]; do sleep 0.1; i=$((i+1)); done
	case "$r1|$r2" in
	*u=*"|"*) bad "J. read through the wrong CA before the reload: $r1";;
	*"|"*-*|*"|") bad "J. before '$r1', after the reload '$r2'; $(grep reload j.log | tr '\n' '|')";;
	*"|"*u=*) grep -q "reloaded: .*the upstreams' TLS files" j.log &&
		grep -q "reload failed: .*upstream_tls_ca.* - nothing changed" j.log &&
		ok "J. a reload read the CA again - reads came through; a file that does not load changed nothing" ||
		bad "J. reads came, the log: $(grep reload j.log | tr '\n' '|')";;
	*) bad "J. before '$r1', after the reload '$r2'; $(grep reload j.log | tr '\n' '|')";;
	esac
fi
[ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; }; PID=

# I: the config
refused() { # refused <leg> <expected text> <url> [lines...] - not "l": conf's loop takes it
	rl=$1 rw=$2; shift 2
	conf "$rl" "$@"
	"$BIN" -C -f "$D/$rl.conf" > "$D/$rl.out" 2>&1 && { bad "I. $rl: started"; return; }
	grep -q "$rw" "$D/$rl.out" && ok "I. $rl refused: $rw" || bad "I. $rl refused without the reason: $(grep -v 'configuration is invalid' "$D/$rl.out" | tail -1)"
}
refused mixed "all redis:// or all rediss://" "rediss://127.0.0.1:$PU, redis://127.0.0.1:$PN"
refused certnokey "upstream_tls_cert and upstream_tls_key" "rediss://127.0.0.1:$PC" "upstream_tls_cert = $D/client.pem"
refused caverifyno "upstream_tls_ca has no use" "rediss://127.0.0.1:$PV" "upstream_tls_ca = $D/ca.pem" "upstream_tls_verify = no"
refused plainkeys "need a rediss:// upstream" "redis://127.0.0.1:$PU" "upstream_tls_ca = $D/ca.pem"
echo "uptlstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
