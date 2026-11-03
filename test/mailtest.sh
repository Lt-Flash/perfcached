#!/bin/sh
# mailtest.sh - S352a: the alert mailer's transport, against test/fakesmtp.py
# on loopback.
#  1. -C: a [mail] section in a build without an SMTP client is refused with
#     the reason; with one, each bad value is refused by name - a URL with
#     credentials, a path, another scheme, a bad from/to, a user without
#     [secrets] mail, an unreadable ca_file - and a good one passes, -E
#     masking the password
#  2. mailtest needs privilege
#  3. STARTTLS + AUTH: the mail arrives over TLS, authenticated, to every
#     recipient, with the subject prefix; the verb answers the server's 250
#  4. a wrong password: {sent: false} with the server's 535, counted, kept
#     as last_error
#  5. smtps:// (TLS from the first byte) delivers too
#  6. smtp:// to a server that offers no STARTTLS: refused, nothing sent in
#     the clear
#  7. a dead server: refused at once; a stalled one: refused at timeout_s,
#     not later - the verb's wait is bounded
#  8. the password appears nowhere - not in the log, /stats or -E
#  9. tls_verify = no: a server whose certificate cannot be verified (no
#     ca_file for the fake's self-signed one) is refused by default, and
#     reached - still over TLS - with tls_verify = no, which -C warns of
# A binary without the SMTP client is used for (1)'s refusal, and a libcurl
# build of this tree for the rest (built here when libcurl's development
# files are installed; SKIPPED, loudly, when not).
# Usage: test/mailtest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
export PYTHONPATH="$T${PYTHONPATH:+:$PYTHONPATH}"
D=$(mktemp -d /var/tmp/pcmail.XXXXXX)
PID= F1= F2= F3= F4=
trap 'for p in $PID $F1 $F2 $F3 $F4; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); echo "  ok   $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL $1"; }
skp()  { skip=$((skip+1)); echo "  SKIP $1"; }
PW=Mx7-pw-never-shown-41
P_ST=17831 P_TL=17832 P_PL=17833 P_DEAD=17834 TCP=17836 HTTP=17837
for p in $P_ST $P_TL $P_PL $P_DEAD $TCP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "mailtest: port $p already bound" >&2; exit 1; }
done

base_conf() { # base_conf <file> - a node, plus whatever [mail] follows
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 32
[secrets]
client = mail-client-secret
enable = mail-enable-secret
mail = $PW
[listen]
tcp = 127.0.0.1:$TCP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 8
EOF
	chmod 600 "$1"
}

# ---- 1a. a build without the client refuses [mail] -----------------------------
base_conf "$D/c1.conf"
printf '[mail]\nsmtp = smtp://127.0.0.1:%d\nuser = alerts\nfrom = pc@example.net\nto = ops@example.net\n' $P_ST >> "$D/c1.conf"
"$BIN" -C -f "$D/c1.conf" > "$D/c1.out" 2>&1; rc=$?
MBIN=
if grep -q "has no SMTP client" "$D/c1.out"; then
	[ $rc -ne 0 ] && ok "1. a build without an SMTP client refuses [mail], saying how to get one" ||
		bad "1. the refusal returned rc 0"
	if pkg-config --exists libcurl 2>/dev/null; then
		echo "--- building this tree with PC_MAIL=libcurl"
		mkdir -p "$D/t"
		(cd "$T/.." && tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' --exclude=config.mk . 2>/dev/null) |
			(cd "$D/t" && tar -xf -)
		if make -s -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" PC_MAIL=libcurl perfcached > "$D/build.log" 2>&1; then
			MBIN="$D/t/perfcached"
		else
			bad "the PC_MAIL=libcurl build failed: $(grep -m3 -E 'error|Error' "$D/build.log" | tr '\n' ' ')"
		fi
	else
		skp "libcurl's development files are not installed - the transport is not exercised here (libcurl4-openssl-dev / curl-dev)"
	fi
else
	skp "1. the binary under test has an SMTP client - the no-client refusal is not exercised by it"
	MBIN=$BIN
fi
if [ -z "$MBIN" ]; then
	echo "mailtest: $pass passed, $fail failed, $skip skipped"
	[ $fail -eq 0 ]; exit
fi

# ---- 1b. -C with the client ----------------------------------------------------
cc_case() { # cc_case <label> <want-in-output> <mail-lines...>
	lbl=$1; want=$2; shift 2
	base_conf "$D/cc.conf"
	printf '[mail]\n' >> "$D/cc.conf"
	for l in "$@"; do printf '%s\n' "$l" >> "$D/cc.conf"; done
	"$MBIN" -C -f "$D/cc.conf" > "$D/cc.out" 2>&1; r=$?
	if [ $r -ne 0 ] && grep -q -- "$want" "$D/cc.out"; then ok "1. -C refuses $lbl"
	else bad "1. -C on $lbl: rc $r, $(grep -m1 -iE 'error|ok' "$D/cc.out")"; fi
}
G="from = pc@example.net"
cc_case "credentials in the URL" "no credentials in the URL" "smtp = smtp://u:p@127.0.0.1:25" "$G" "to = ops@example.net"
cc_case "a path in the URL" "a host and a port only" "smtp = smtp://127.0.0.1:25/x" "$G" "to = ops@example.net"
cc_case "another scheme" "want smtp://" "smtp = https://127.0.0.1:25" "$G" "to = ops@example.net"
cc_case "a display name in to" "not an address" "smtp = smtp://127.0.0.1:25" "$G" "to = Ops <ops@example.net>"
cc_case "a from without a domain" "not an address" "smtp = smtp://127.0.0.1:25" "from = pc" "to = ops@example.net"
cc_case "[mail] without to" "needs to =" "smtp = smtp://127.0.0.1:25" "$G"
cc_case "an unreadable ca_file" "cannot be read" "smtp = smtp://127.0.0.1:25" "$G" "to = ops@example.net" "ca_file = /nonexistent/ca.pem"
base_conf "$D/cc.conf"; sed -i '/^mail = /d' "$D/cc.conf"
printf '[mail]\nsmtp = smtp://127.0.0.1:25\nuser = alerts\n%s\nto = ops@example.net\n' "$G" >> "$D/cc.conf"
"$MBIN" -C -f "$D/cc.conf" > "$D/cc.out" 2>&1 && bad "1. a user without [secrets] mail passed -C" ||
	{ grep -q "no \[secrets\] mail" "$D/cc.out" && ok "1. -C refuses a user without [secrets] mail" || bad "1. user without password: $(tail -1 "$D/cc.out")"; }
cc_case "tls_verify = maybe" "tls_verify: yes|no" "smtp = smtp://127.0.0.1:25" "$G" "to = ops@example.net" "tls_verify = maybe"
base_conf "$D/cc.conf"
printf '[mail]\nsmtp = smtp://127.0.0.1:25\n%s\nto = ops@example.net\ntls_verify = no\n' "$G" >> "$D/cc.conf"
"$MBIN" -C -f "$D/cc.conf" > "$D/cc.out" 2>&1 && grep -q "tls_verify = no: mail is encrypted, but the server's certificate and name are NOT checked" "$D/cc.out" &&
	ok "9. -C passes tls_verify = no and warns that the server is not checked" || bad "9. -C on tls_verify = no: $(tail -2 "$D/cc.out" | tr '\n' ' ')"
"$MBIN" -E -f "$D/cc.conf" 2>/dev/null | grep -qx "tls_verify = no" && ok "9. -E shows tls_verify = no" || bad "9. -E: no tls_verify line"
base_conf "$D/cc.conf"
printf '[mail]\nsmtp = smtps://127.0.0.1:465\nuser = alerts\n%s\nto = a@example.net, b@example.net\nlevel = critical\n' "$G" >> "$D/cc.conf"
"$MBIN" -C -f "$D/cc.conf" > "$D/cc.out" 2>&1 && ok "1. a good [mail] passes -C" || bad "1. a good [mail] refused: $(tail -2 "$D/cc.out" | tr '\n' ' ')"
"$MBIN" -E -f "$D/cc.conf" > "$D/ce.out" 2>&1
grep -q "mail = \*\*\*\*\*\*\*\* (len ${#PW})" "$D/ce.out" && ! grep -q -- "$PW" "$D/ce.out" &&
	ok "1. -E masks the mail secret and shows its length" || bad "1. -E and the mail secret: $(grep -m1 'mail' "$D/ce.out")"

# ---- the fakes ---------------------------------------------------------------
if command -v openssl > /dev/null 2>&1; then
	openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/key.pem" -out "$D/cert.pem" -days 2 \
		-subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > "$D/openssl.log" 2>&1
fi
if [ ! -s "$D/cert.pem" ]; then
	python3 - "$D" <<'PY' 2>"$D/certpy.log"
import datetime, ipaddress, sys
from cryptography import x509
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
d = sys.argv[1]
k = rsa.generate_private_key(public_exponent=65537, key_size=2048, backend=default_backend())
n = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"localhost")])
now = datetime.datetime.utcnow()
c = (x509.CertificateBuilder().subject_name(n).issuer_name(n).public_key(k.public_key())
     .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
     .not_valid_after(now + datetime.timedelta(days=2))
     .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(u"127.0.0.1")), x509.DNSName(u"localhost")]), False)
     .sign(k, hashes.SHA256(), default_backend()))
open(d + "/cert.pem", "wb").write(c.public_bytes(serialization.Encoding.PEM))
open(d + "/key.pem", "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
PY
fi
if [ ! -s "$D/cert.pem" ]; then
	skp "no openssl and no python cryptography - the TLS cases cannot run"
	echo "mailtest: $pass passed, $fail failed, $skip skipped"
	[ $fail -eq 0 ]; exit
fi
fake() { # fake <port> <dir> <args...>
	p=$1; d=$2; shift 2
	mkdir -p "$d"
	python3 "$T/fakesmtp.py" --port "$p" --dir "$d" --cert "$D/cert.pem" --key "$D/key.pem" "$@" > "$d.log" 2>&1 &
	FP=$!
	i=0; while [ $i -lt 50 ] && [ ! -f "$d/ready" ]; do sleep 0.1; i=$((i+1)); done
}
fake $P_ST "$D/st" --mode starttls --user alerts --password "$PW"; F1=$FP
fake $P_TL "$D/tl" --mode tls --user alerts --password "$PW"; F2=$FP
fake $P_PL "$D/pl" --mode plain; F3=$FP

start_node() { # start_node <smtp url> [extra mail line] [noca] - one node, waits ready
	[ -n "$PID" ] && { kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
	base_conf "$D/n.conf"
	printf '[mail]\nsmtp = %s\nuser = alerts\nfrom = pc@example.net\nto = ops@example.net, oncall@example.net\ntimeout_s = 5\n' \
		"$1" >> "$D/n.conf"
	[ "${3:-}" = noca ] || printf 'ca_file = %s\n' "$D/cert.pem" >> "$D/n.conf"
	[ -n "${2:-}" ] && printf '%s\n' "$2" >> "$D/n.conf"
	r0=$(grep -c "perfcached ready" "$D/n.log" 2>/dev/null || echo 0)   # the log is shared
	"$MBIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 100 ] && [ "$(grep -c "perfcached ready" "$D/n.log")" -le "$r0" ]; do sleep 0.1; i=$((i+1)); done
	: > "$D/n.ready"
}
mt() { # mt <1 to enable first> - prints {"sent":..,"reply":..} or the error
	python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),30))
if sys.argv[2]=="1":
    pcnative.call(f,"enable",secret="mail-enable-secret")
r=pcnative.call(f,"mailtest")
print(json.dumps(r.get("result", r.get("error"))))' $TCP "$1" 2>&1
}
mstat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
print(json.dumps(r["mail"][sys.argv[1]]))' "$1" 2>/dev/null; }

# ---- 2 + 3. STARTTLS, AUTH ------------------------------------------------------
start_node "smtp://127.0.0.1:$P_ST"
[ "$(mstat ready)" = true ] && ok "the mailer is ready (/stats mail.ready)" || bad "mail.ready: $(mstat ready)"
case "$(mt 0)" in *rivileg*|*enable*) ok "2. mailtest without privilege is refused";; *) bad "2. unprivileged mailtest: $(mt 0)";; esac
R=$(mt 1)
case "$R" in *'"sent": true'*'250 queued as fake'*) ok "3. STARTTLS: mailtest answers the server's 250 ($R)";;
	*) bad "3. STARTTLS mailtest: $R";; esac
M=$(ls "$D/st"/*.json 2>/dev/null | head -1)
if [ -n "$M" ]; then
	python3 - "$M" <<'PY' > "$D/m.chk"
import json, sys
m = json.load(open(sys.argv[1]))
d = m["data"]
print("tls" if m["tls"] else "NOTLS", m["user"], ",".join(sorted(m["rcpt"])),
      "subj" if "\nSubject: perfcached " in "\n" + d and "test mail from" in d else "NOSUBJ")
PY
	[ "$(cat "$D/m.chk")" = "tls alerts <oncall@example.net>,<ops@example.net> subj" ] &&
		ok "3. it arrived over TLS, authenticated, to both recipients, with the subject prefix" ||
		bad "3. the recorded mail: $(cat "$D/m.chk")"
else
	bad "3. no mail reached the fake"
fi
[ "$(mstat sent)" = 1 ] && ok "3. /stats mail.sent 1" || bad "3. mail.sent $(mstat sent)"

# ---- 4. a wrong password ----------------------------------------------------------
kill $F1 2>/dev/null; wait $F1 2>/dev/null
fake $P_ST "$D/st2" --mode starttls --user alerts --password "not-$PW"; F1=$FP
R=$(mt 1)
case "$R" in *'"sent": false'*535*) ok "4. a wrong password: sent false with the server's 535 ($R)";; *) bad "4. wrong password: $R";; esac
[ "$(mstat failed)" = 1 ] && case "$(mstat last_error)" in *535*) true;; *) false;; esac &&
	ok "4. counted (mail.failed 1) and kept as last_error" || bad "4. failed $(mstat failed), last_error $(mstat last_error)"

# ---- 5. smtps:// ---------------------------------------------------------------------
start_node "smtps://127.0.0.1:$P_TL"
R=$(mt 1)
case "$R" in *'"sent": true'*) ok "5. smtps:// delivers ($R)";; *) bad "5. smtps: $R";; esac
[ -n "$(ls "$D/tl"/*.json 2>/dev/null)" ] && ok "5. the TLS fake recorded it" || bad "5. nothing at the TLS fake"

# ---- 6. no STARTTLS on offer --------------------------------------------------------
start_node "smtp://127.0.0.1:$P_PL"
R=$(mt 1)
case "$R" in *'"sent": false'*) ok "6. smtp:// to a server without STARTTLS is refused ($R)";; *) bad "6. no-STARTTLS server: $R";; esac
[ -z "$(ls "$D/pl"/*.json 2>/dev/null)" ] && ok "6. nothing was sent in the clear" || bad "6. A MAIL WENT IN THE CLEAR"

# ---- 7. a dead server ----------------------------------------------------------------
start_node "smtp://127.0.0.1:$P_DEAD"
s0=$(date +%s); R=$(mt 1); s1=$(date +%s)
case "$R" in *'"sent": false'*) [ $((s1 - s0)) -le 12 ] && ok "7. a dead server: refused in $((s1 - s0)) s ($R)" ||
	bad "7. a dead server took $((s1 - s0)) s";; *) bad "7. dead server: $R";; esac

fake $P_DEAD "$D/slow" --mode starttls --delay 8; F4=$FP
start_node "smtp://127.0.0.1:$P_DEAD"
s0=$(date +%s); R=$(mt 1); s1=$(date +%s)
case "$R" in *'"sent": false'*) [ $((s1 - s0)) -ge 4 ] && [ $((s1 - s0)) -le 9 ] &&
	ok "7. a stalled server: refused after $((s1 - s0)) s - timeout_s 5 bounds it ($R)" ||
	bad "7. a stalled server answered after $((s1 - s0)) s";; *) bad "7. stalled server: $R";; esac

# ---- 9. tls_verify = no -------------------------------------------------------------
kill $F1 2>/dev/null; wait $F1 2>/dev/null
fake $P_ST "$D/st3" --mode starttls --user alerts --password "$PW"; F1=$FP
start_node "smtp://127.0.0.1:$P_ST" "" noca
R=$(mt 1)
case "$R" in *'"sent": false'*ertificate*) ok "9. no ca_file for a self-signed server: refused by default ($R)";;
	*) bad "9. default verification against a self-signed server: $R";; esac
[ -z "$(ls "$D/st3"/*.json 2>/dev/null)" ] && ok "9. and nothing reached it" || bad "9. A MAIL WENT TO AN UNVERIFIED SERVER"
start_node "smtp://127.0.0.1:$P_ST" "tls_verify = no" noca
R=$(mt 1)
case "$R" in *'"sent": true'*) ok "9. tls_verify = no: delivered ($R)";; *) bad "9. tls_verify = no: $R";; esac
M=$(ls "$D/st3"/*.json 2>/dev/null | head -1)
[ -n "$M" ] && python3 -c 'import json,sys;sys.exit(0 if json.load(open(sys.argv[1]))["tls"] else 1)' "$M" &&
	ok "9. still over TLS (STARTTLS)" || bad "9. not over TLS, or not delivered"
grep -q "the server's certificate is NOT verified" "$D/n.log" && ok "9. the start says the certificate is not verified" ||
	bad "9. no start notice of tls_verify = no"

# ---- 8. the password, nowhere ---------------------------------------------------------
python3 -c 'import urllib.request;print(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read().decode())' > "$D/stats.json" 2>/dev/null
n=$(cat "$D/n.log" "$D/stats.json" "$D/ce.out" | grep -c -- "$PW")
[ "$n" = 0 ] && ok "8. the password appears in neither the log, /stats nor -E" || bad "8. THE PASSWORD LEAKED ($n lines)"

kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "mailtest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
