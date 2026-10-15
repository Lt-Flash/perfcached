#!/bin/sh
# upsecrettest.sh - S383 (0.6.1): the upstream's password belongs to the
# read-through collection (`upstream_password`), so one perfcached fronts
# several Redis servers, each with its own - reached by its clients through
# the one server.  Two fakes (test/fakeupstream.py), each with its own
# password; three collections over RESP (SELECT 0, 1, 2):
#   ca -> fake A with A's password, cb -> fake B with B's, cc -> fake A
#   with a WRONG password.
#  1. ca reads A's value, cb reads B's - each with its own password
#  2. cc's AUTH is refused by A: its read answers an error, the log says so
#  3. no password is in the log, /stats, the page or the -E dump
#  4. config: [secrets] upstream is refused, saying where it moved; an
#     upstream_password without an upstream, or empty, is refused
#  and nothing is written to either fake.
# Fail-first: 0.6.0.1 refuses upstream_password as an unknown key.
# Usage: test/upsecrettest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcupsec.XXXXXX)
PA=19851 PB=19852 TCP=19853 RESP=19854 HTTP=19855
FA= FB= PID=
trap 'for p in $PID $FA $FB; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PA $PB $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upsecrettest: port $p already bound" >&2; exit 1; }
done

mkdir -p "$D/a" "$D/b"
echo '{"a:1": {"type": "string", "value": "from-a"}}' > "$D/a/data.json"
echo '{"b:1": {"type": "string", "value": "from-b"}}' > "$D/b/data.json"
echo '{"password": "pw-alpha-1", "resp3": true}' > "$D/a/ctl.json"
echo '{"password": "pw-bravo-2", "resp3": true}' > "$D/b/ctl.json"
python3 "$T/fakeupstream.py" $PA "$D/a" > "$D/fa.log" 2>&1 & FA=$!
python3 "$T/fakeupstream.py" $PB "$D/b" > "$D/fb.log" 2>&1 & FB=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/a/ready" ] && [ -f "$D/b/ready" ]; }; do sleep 0.1; i=$((i+1)); done

head_conf() {
	cat <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = upsec-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
resp_collections = 0:ca, 1:cb, 2:cc
http = 127.0.0.1:$HTTP
plaintext = loopback
C
}
{
	head_conf
	cat <<C
[collection ca]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PA
upstream_password = pw-alpha-1
upstream_prefixes = a:*
[collection cb]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PB
upstream_password = pw-bravo-2
upstream_prefixes = b:*
[collection cc]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PA
upstream_password = wrong-pass-3
upstream_prefixes = a:*
upstream_timeout_ms = 500
C
} > "$D/n.conf"
chmod 600 "$D/n.conf"

get() { # get <db> <key> - the RESP reply's first line, or its value
	python3 - $RESP "$1" "$2" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
f = s.makefile("rb")
def cmd(*a):
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    l = f.readline()
    if l.startswith(b"$") and not l.startswith(b"$-1"):
        return f.read(int(l[1:]) + 2)[:-2].decode()
    return l.decode().strip()
cmd(b"SELECT", sys.argv[2].encode())
print(cmd(b"GET", sys.argv[3].encode()))
PY
}

# ---- 1, 2. each collection with its own password --------------------------------------
mkdir -p "$D/state"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if grep -q "perfcached ready" "$D/n.log"; then
	[ "$(get 0 a:1)" = from-a ] && ok "1. collection ca reads fake A's value with A's password" || bad "1. ca: '$(get 0 a:1)'"
	[ "$(get 1 b:1)" = from-b ] && ok "1. collection cb reads fake B's value with B's password" || bad "1. cb: '$(get 1 b:1)'"
	r=$(get 2 a:1)
	case "$r" in -*) ok "2. collection cc, a wrong password for A: its read answers an error ($r)";;
		*) bad "2. cc answered '$r'";; esac
	grep -q "upstream cc: AUTH refused by 127.0.0.1:$PA" "$D/n.log" && ok "2. the log says A refused cc's AUTH" ||
		bad "2. log: $(grep -m2 -i 'auth' "$D/n.log")"
	curl -s "http://127.0.0.1:$HTTP/stats" > "$D/stats.json" 2>/dev/null
	curl -s "http://127.0.0.1:$HTTP/" > "$D/page.html" 2>/dev/null
	kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
else
	bad "the node did not start: $(grep -m2 -E 'ERROR|CRIT' "$D/n.log")"
fi

# ---- 3. no password anywhere ------------------------------------------------------------
"$BIN" -E -f "$D/n.conf" > "$D/dump.txt" 2>&1
[ -s "$D/stats.json" ] && [ -s "$D/page.html" ] && [ -s "$D/dump.txt" ] && [ -s "$D/n.log" ] &&
	! grep -qE 'pw-alpha-1|pw-bravo-2|wrong-pass-3' "$D/n.log" "$D/stats.json" "$D/page.html" "$D/dump.txt" &&
	ok "3. no password in the log, /stats, the page or the -E dump" ||
	bad "3. a password leaked: $(grep -lE 'pw-alpha-1|pw-bravo-2|wrong-pass-3' "$D"/n.log "$D"/stats.json "$D"/page.html "$D"/dump.txt 2>/dev/null)"

# ---- 4. config ------------------------------------------------------------------------------
cfgerr() { # cfgerr <name> <expected message> <extra lines...> - -C refuses it, saying so
	n=$1 msg=$2; shift 2
	{ head_conf; for l in "$@"; do echo "$l"; done; } > "$D/$n.conf"; chmod 600 "$D/$n.conf"
	"$BIN" -C -f "$D/$n.conf" > "$D/$n.out" 2>&1
	[ $? -ne 0 ] && grep -qF "$msg" "$D/$n.out"
}
cfgerr old "[secrets] upstream moved in 0.6.1: write upstream_password = <it> in each read-through [collection]" \
	"[collection ca]" "buckets_log2 = 8" "upstream = redis://127.0.0.1:$PA" "upstream_prefixes = a:*" \
	"[secrets]" "upstream = pw-alpha-1" &&
	ok "4. [secrets] upstream is refused, saying where it moved" || bad "4. old secret: $(tail -2 "$D/old.out")"
cfgerr alone "[collection ca] upstream_password without an upstream" \
	"[collection ca]" "buckets_log2 = 8" "upstream_password = pw-alpha-1" &&
	ok "4. upstream_password without an upstream is refused" || bad "4. alone: $(tail -2 "$D/alone.out")"
cfgerr empty "empty upstream_password" \
	"[collection ca]" "buckets_log2 = 8" "upstream = redis://127.0.0.1:$PA" "upstream_prefixes = a:*" "upstream_password =" &&
	ok "4. an empty upstream_password is refused" || bad "4. empty: $(tail -2 "$D/empty.out")"

w=$(cat "$D"/a/writes "$D"/b/writes 2>/dev/null | wc -l)
[ "$w" = 0 ] && ok "nothing was written to either fake" || bad "$w writes upstream"
echo "upsecrettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
