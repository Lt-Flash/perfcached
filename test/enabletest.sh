#!/bin/sh
# enabletest.sh — S129: DDL needs a PRIVILEGED connection, not just the
# client secret.
#
# WHY THIS EXISTS.  `allow_create` answers "may this NODE originate DDL".
# It cannot answer "may THIS LINK", because apps, ops tooling and perfcli
# all present the same [secrets] client value - so turning the node gate
# on hands create/drop/resize/rename (and restore) to every application
# connection.  A distinct [secrets] enable raises a bit on the
# connection; it dies with the connection and never crosses the wire.
#
# usage: test/enabletest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

D=$(mktemp -d /var/tmp/pcen.XXXXXX)
P1=; P2=
trap 'for p in $P1 $P2; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
if ss -ltn 2>/dev/null | grep -qE ":1799[12][[:space:]]"; then
	echo "enabletest: port 17991/17992 already bound" >&2; exit 1
fi

# node 1: allow_create ON, enable secret SET.  node 2: allow_create ON,
# NO enable secret - the fail-closed case.
conf() {
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = 128
[secrets]
client = en-client-secret
cluster = en-cluster-secret
$2
[listen]
tcp = 127.0.0.1:1799$1
plaintext = loopback
[collection base]
buckets_log2 = 10
CONF
}
conf 1 "enable = en-privilege-secret"
conf 2 ""
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null &&
		grep -q "perfcached ready" "$D/n2.log" 2>/dev/null && break
	sleep 0.1; i=$((i + 1))
done
[ $i -lt 200 ] || { echo "  daemons did not start"; exit 1; }

# One CONNECTION carrying a sequence of requests - the privilege bit is
# per connection, so each case must be one session, not one call.
seq_json() { # seq_json <port> <json-line>...
	python3 - "$@" <<'PYEOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb")
for i, raw in enumerate(sys.argv[2:]):
    r = json.loads(raw); r["id"] = i + 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    print(f.readline().decode().strip())
s.close()
PYEOF
}

R=$(seq_json 17991 '{"method":"create","params":{"col":"unpriv","buckets_log2":8}}')
case "$R" in *"privileged connection"*) ok "an unprivileged connection is refused DDL, and the message says why";;
  *) bad "unprivileged create: $R";; esac

R=$(seq_json 17991 '{"method":"enable","params":{"secret":"wrong-one"}}')
case "$R" in *"wrong enable secret"*) ok "a wrong enable secret is refused";; *) bad "wrong secret: $R";; esac

R=$(seq_json 17991 '{"method":"enable","params":{"secret":"en-privilege-secret"}}' \
                   '{"method":"create","params":{"col":"priv","buckets_log2":8}}')
case "$R" in *'"privileged":true'*) ok "enable with the right secret raises privilege";; *) bad "enable: $R";; esac
case "$R" in *'"created":true'*) ok "and DDL then succeeds on that connection";; *) bad "create after enable: $R";; esac

R=$(seq_json 17991 '{"method":"enable","params":{"secret":"en-privilege-secret"}}' \
                   '{"method":"disable","params":{}}' \
                   '{"method":"create","params":{"col":"after","buckets_log2":8}}')
case "$R" in *"privileged connection"*) ok "disable drops it again, and DDL is refused after";; *) bad "after disable: $R";; esac

# THE ONE THAT MATTERS: privilege must not survive the connection.
R=$(seq_json 17991 '{"method":"create","params":{"col":"leaked","buckets_log2":8}}')
case "$R" in *"privileged connection"*) ok "a NEW connection is unprivileged - the bit did not leak";;
  *) bad "privilege leaked to a new connection: $R";; esac

# fail-closed: node 2 has allow_create on but no enable secret at all
R=$(seq_json 17992 '{"method":"enable","params":{"secret":"anything"}}')
case "$R" in *"no [secrets] enable"*) ok "with no enable secret, enable itself fails (fail-closed)";;
  *) bad "node 2 enable: $R";; esac
R=$(seq_json 17992 '{"method":"create","params":{"col":"nope","buckets_log2":8}}')
case "$R" in *"unreachable"*) ok "and DDL is unreachable there however allow_create is set";;
  *) bad "node 2 create: $R";; esac
grep -q "allow_create is ON and no \[secrets\] enable" "$D/n2.log" \
	&& ok "the daemon warned at startup about that exact combination" \
	|| bad "no startup warning on node 2"

R=$(seq_json 17991 '{"method":"restore","params":{"col":"base","records":[]}}')
case "$R" in *"privileged connection"*) ok "restore is in the privileged set (blast radius, not the word DDL)";;
  *) bad "unprivileged restore: $R";; esac

R=$(seq_json 17991 '{"method":"get","params":{"col":"base","key":"k"}}')
case "$R" in *'"found"'*) ok "ordinary verbs are untouched by any of this";; *) bad "plain get: $R";; esac

echo "enabletest: $pass passed, $fail failed"
[ $fail -eq 0 ]
