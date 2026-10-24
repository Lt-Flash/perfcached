#!/bin/sh
# reloadtest.sh - 12nw (0.7.1): a configuration reload.  SIGHUP, `perfcached
# -f <conf> -H <pid>` (what the units' ExecReload runs) and the privileged
# `reload` verb read the file again; when every key that changed is one a
# running daemon can take - resp_allow, http_allow, the http / resp /
# enable / client secrets, log_level, slowlog_usec - it is applied, and
# when ANY other key changed the reload is refused whole, naming the keys,
# and nothing changes.  Cases:
#   A. nothing changed: -H answers so, exit 0
#   B. the http token, the RESP password and the enable secret, one reload:
#      the old ones refused after it, the new ones taken
#   C. resp_allow narrowed past this address: the RESP door refuses it;
#      widened again: taken
#   D. the client secret: a new perfcli handshake needs the new one
#   E. slowlog_usec 10 s -> 0: the next command is in the slow log
#   F. a key that cannot reload ([daemon] workers) beside one that can (the
#      token): refused, exit 1, the message names [daemon] workers, and the
#      token did NOT change
#   G. a file that does not load: refused, exit 1, the daemon untouched
#   H. kill -HUP: the daemon lives (before 0.7.1 it died) and reloads
#   I. the verb: refused without privilege, applied with it; stats.reload
#      counts applied and refused
# Fail-first: before 0.7.1 there is no -H and SIGHUP ends the daemon.
# Usage: test/reloadtest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}; CLI=${2:-./perfcli}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
D=$(mktemp -d /var/tmp/pcreload.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
TCP=16961 RESP=16962 HTTP=16963
unset RUNTIME_DIRECTORY                 # the outcome goes to state_dir here

# conf <client> <enable> <http> <resp> <resp_allow> <slowlog_usec> [workers]
conf() {
	cat > "$D/n.conf" <<C
[daemon]
workers = ${7:-2}
log_level = notice
slowlog_usec = $6
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = $1
enable = $2
http = $3
resp = $4
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
resp_allow = $5
plaintext = never
[collection 0]
buckets_log2 = 10
C
	chmod 600 "$D/n.conf"
}
mkdir -p "$D/state"
conf rl-client-1 rl-enable-1 rl-token-1 rl-resp-1 127.0.0.1/32 10000000
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "daemon did not start"; tail -5 "$D/n.log"; exit 1; }

reload() { "$BIN" -f "$D/n.conf" -H $P > "$D/h.out" 2>&1; echo $?; }
http() { # http <token> -> status of GET /stats
	curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $1" "http://127.0.0.1:$HTTP/stats"
}
resp_auth() { # resp_auth <password> -> the reply's first line
	python3 - "$RESP" "$1" <<'PY'
import socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
    s.sendall(b"*2\r\n$4\r\nAUTH\r\n$%d\r\n%s\r\n" % (len(sys.argv[2]), sys.argv[2].encode()))
    r = s.recv(200).split(b"\r\n")[0].decode()
    print(r if r else "CLOSED")
except Exception as e:
    print("CLOSED %s" % type(e).__name__)
PY
}
cli() { # cli <client secret> [-E enable] args...
	sec=$1; shift
	timeout 30 "$CLI" -q -h 127.0.0.1 -p $TCP -a "$sec" "$@" 2>&1
}

# ---- A. nothing changed ------------------------------------------------------------------
rc=$(reload)
[ "$rc" = 0 ] && grep -q "reload: nothing changed" "$D/h.out" && ok "A. an unchanged file: 'nothing changed', exit 0" || bad "A. rc $rc: $(cat "$D/h.out")"
[ "$(http rl-token-1)" = 200 ] && [ "$(resp_auth rl-resp-1)" = "+OK" ] && ok "A. the token and the RESP password as started" || bad "A. token $(http rl-token-1), resp $(resp_auth rl-resp-1)"

# ---- B. three secrets in one reload --------------------------------------------------------
conf rl-client-1 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.1/32 10000000
rc=$(reload)
[ "$rc" = 0 ] && grep -q "reloaded:" "$D/h.out" && grep -q "the HTTP token" "$D/h.out" && grep -q "the RESP password" "$D/h.out" &&
	grep -q "the enable secrets" "$D/h.out" && ok "B. reloaded, naming the token, the RESP password and the enable secrets" || bad "B. rc $rc: $(cat "$D/h.out")"
[ "$(http rl-token-1)" = 401 ] && [ "$(http rl-token-2)" = 200 ] && ok "B. the old token 401, the new one 200" || bad "B. token: old $(http rl-token-1), new $(http rl-token-2)"
r1=$(resp_auth rl-resp-1); r2=$(resp_auth rl-resp-2)
case "$r1" in -*) ;; *) r1=BAD;; esac
[ "$r1" != BAD ] && [ "$r2" = "+OK" ] && ok "B. the old RESP password refused, the new one taken" || bad "B. resp: old '$(resp_auth rl-resp-1)', new '$r2'"
o=$(cli rl-client-1 -E rl-enable-1 reload); n=$(cli rl-client-1 -E rl-enable-2 reload)
echo "$n" | grep -q '"applied"' && ! echo "$o" | grep -q '"applied":true' && ok "B. the old enable secret refused, the new one privileged" || bad "B. enable: old '$o', new '$n'"

# ---- C. resp_allow ----------------------------------------------------------------------------
conf rl-client-1 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.9/32 10000000
rc=$(reload)
r=$(resp_auth rl-resp-2)
[ "$rc" = 0 ] && grep -q "resp_allow" "$D/h.out" && case "$r" in CLOSED*) true;; *) false;; esac &&
	ok "C. resp_allow narrowed to 127.0.0.9: this address is refused at accept ($r)" || bad "C. rc $rc, resp '$r': $(cat "$D/h.out")"
conf rl-client-1 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.0/8 10000000
rc=$(reload)
[ "$rc" = 0 ] && [ "$(resp_auth rl-resp-2)" = "+OK" ] && ok "C. widened to 127.0.0.0/8: taken again" || bad "C. widened: rc $rc, resp '$(resp_auth rl-resp-2)'"

# ---- D. the client secret ------------------------------------------------------------------------
[ "$(cli rl-client-1 ping)" != "" ] && cli rl-client-1 ping | grep -q pong && ok "D. perfcli with the client secret as started" || bad "D. before: $(cli rl-client-1 ping)"
conf rl-client-2 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.0/8 10000000
rc=$(reload)
[ "$rc" = 0 ] && grep -q "the client secrets" "$D/h.out" && ok "D. reloaded: the client secrets" || bad "D. rc $rc: $(cat "$D/h.out")"
cli rl-client-2 ping | grep -q pong && ! cli rl-client-1 ping | grep -q pong &&
	ok "D. a new handshake: the new secret taken, the old one refused" || bad "D. new '$(cli rl-client-2 ping)', old '$(cli rl-client-1 ping)'"

# ---- E. slowlog_usec ------------------------------------------------------------------------------
slowlen() { curl -s -H "Authorization: Bearer $1" "http://127.0.0.1:$HTTP/stats" | python3 -c 'import json,sys; print(len(json.load(sys.stdin).get("slowlog") or []))' 2>/dev/null; }
s0=$(slowlen rl-token-2)
conf rl-client-2 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.0/8 0
rc=$(reload)
cli rl-client-2 set 0 slowkey v > /dev/null
s1=$(slowlen rl-token-2)
[ "$rc" = 0 ] && grep -q "slowlog_usec" "$D/h.out" && [ "${s1:-0}" -gt "${s0:-0}" ] 2>/dev/null &&
	ok "E. slowlog_usec 10 s -> 0: the next commands are in the slow log ($s0 -> $s1)" || bad "E. rc $rc, slow log $s0 -> $s1: $(cat "$D/h.out")"

# ---- F. a key that cannot reload ---------------------------------------------------------------------
conf rl-client-2 rl-enable-2 rl-token-3 rl-resp-2 127.0.0.0/8 0 3
rc=$(reload)
[ "$rc" = 1 ] && grep -q "reload refused: \[daemon\] workers changed and cannot be reloaded" "$D/h.out" && grep -q "nothing changed" "$D/h.out" &&
	ok "F. [daemon] workers beside the token: refused, exit 1, naming [daemon] workers" || bad "F. rc $rc: $(cat "$D/h.out")"
[ "$(http rl-token-2)" = 200 ] && [ "$(http rl-token-3)" = 401 ] && ok "F. and the token did NOT change (the refusal is whole)" || bad "F. token after the refusal: old $(http rl-token-2), new $(http rl-token-3)"
grep -q "ERROR: reload refused: \[daemon\] workers" "$D/n.log" && ok "F. the daemon's log says it at ERROR" || bad "F. no ERROR line in the log"

# ---- G. a file that does not load -----------------------------------------------------------------------
conf rl-client-2 rl-enable-2 rl-token-2 rl-resp-2 127.0.0.0/8 0
echo "this line is not a key" >> "$D/n.conf"
rc=$(reload)
[ "$rc" = 1 ] && grep -q "does not load" "$D/h.out" && kill -0 $P 2>/dev/null && [ "$(http rl-token-2)" = 200 ] &&
	ok "G. a broken file: refused by -H before the daemon is asked, exit 1, the daemon untouched" || bad "G. rc $rc: $(cat "$D/h.out")"

# ---- H. kill -HUP ------------------------------------------------------------------------------------------
conf rl-client-2 rl-enable-2 rl-token-4 rl-resp-2 127.0.0.0/8 0
kill -HUP $P
i=0; while [ $i -lt 50 ] && [ "$(http rl-token-4)" != 200 ]; do sleep 0.1; i=$((i+1)); done
kill -0 $P 2>/dev/null && [ "$(http rl-token-4)" = 200 ] && ok "H. SIGHUP: the daemon lives and reloads (token-4 taken)" || bad "H. alive $(kill -0 $P 2>/dev/null && echo yes || echo NO), token-4 $(http rl-token-4)"

# ---- I. the verb and the stats -----------------------------------------------------------------------------
r=$(cli rl-client-2 reload)
echo "$r" | grep -qiE 'privilege|enable' && ! echo "$r" | grep -q '"applied"' && ok "I. the reload verb without privilege: refused" || bad "I. unprivileged: $r"
conf rl-client-2 rl-enable-2 rl-token-5 rl-resp-2 127.0.0.0/8 0
r=$(cli rl-client-2 -E rl-enable-2 reload)
echo "$r" | grep -q '"applied":true' && echo "$r" | grep -q 'the HTTP token' && [ "$(http rl-token-5)" = 200 ] &&
	ok "I. with privilege: applied, answered when done, token-5 taken" || bad "I. privileged: $r"
st=$(curl -s -H "Authorization: Bearer rl-token-5" "http://127.0.0.1:$HTTP/stats" | python3 -c 'import json,sys; r=json.load(sys.stdin)["reload"]; print(r["applied"], r["refused"], r["last_unix"] > 0)' 2>/dev/null)
# applied: A, B, B's privileged verb (nothing changed), C twice, D, E, H, I = 9; refused: F = 1
# (G is refused by -H before the daemon is asked, I's unprivileged verb before it runs)
[ "$st" = "9 1 True" ] && ok "I. stats.reload: 9 applied, 1 refused, the last one stamped" || bad "I. stats.reload: '$st'"
kill -0 $P 2>/dev/null && ok "the daemon ran through every reload" || bad "the daemon is gone: $(tail -5 "$D/n.log")"
grep -E 'ERROR|CRIT' "$D/n.log" | grep -vE 'reload refused|does not load|this line is not a key|expected key = value' | head -3 > "$D/errs"
[ ! -s "$D/errs" ] && ok "no other ERROR or CRIT in the log" || bad "other errors: $(tr '\n' '|' < "$D/errs")"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=
echo "reloadtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
