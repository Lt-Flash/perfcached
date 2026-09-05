#!/bin/sh
# configtest.sh — S5 verification: every validation rule proven to REJECT
# (red fixtures) and a full valid config proven to load (green), plus the
# secrets-never-printed check on -E.  Usage: test/configtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
pass=0 fail=0

expect() { # expect <0|nonzero> <name> <conffile>
	want=$1; name=$2; conf=$3
	"$BIN" -C -f "$conf" >"$D/out" 2>&1
	rc=$?
	if [ "$want" = 0 ] && [ $rc -eq 0 ]; then
		pass=$((pass+1))
	elif [ "$want" != 0 ] && [ $rc -ne 0 ]; then
		pass=$((pass+1))
	else
		fail=$((fail+1))
		echo "FAIL: $name (rc=$rc, wanted $want)"
		sed 's/^/    /' "$D/out"
	fi
}

# ---- green: the full shape ------------------------------------------------
cat > "$D/good.conf" <<'EOF'
[daemon]
workers = 4
log_level = notice

[memory]
arena_mb = 64
backing = own

[secrets]
client = "app secret with spaces and a # inside"
client = second-app-secret
cluster = cluster-only-secret

[listen]
tcp = 0.0.0.0:6479
tcp = 127.0.0.1:6480
unix = /run/perfcached.sock
plaintext = loopback

[collection th]
buckets_log2 = 16
mode = store

[collection billing]
buckets_log2 = 12
EOF
expect 0 "valid config" "$D/good.conf"

# secrets must never appear in the -E dump
"$BIN" -E -f "$D/good.conf" >"$D/dump" 2>&1
if [ $? -eq 0 ] && ! grep -q "cluster-only-secret\|app secret\|second-app-secret" "$D/dump"; then
	pass=$((pass+1))
else
	fail=$((fail+1)); echo "FAIL: -E leaked a secret or errored"; cat "$D/dump"
fi

# ---- red: one broken rule per fixture ------------------------------------
red() { # red <name> <sed-expr applied to good.conf>  OR heredoc via stdin
	name=$1; shift
	if [ $# -gt 0 ]; then
		sed "$1" "$D/good.conf" > "$D/bad.conf"
	else
		cat > "$D/bad.conf"
	fi
	expect 1 "$name" "$D/bad.conf"
}

red "cluster secret equals a client secret" \
	's/^cluster = .*/cluster = second-app-secret/'
red "mode = proxy refused until M5" \
	's/^mode = store/mode = proxy/'
red "mode = shard needs a cluster" \
	's/^mode = store/mode = shard/'
red "mode = ec is refused (the mode was removed in 0.2.0)" \
	's/^mode = store/mode = ec/'
red "eager needs store mode and a cluster" \
	's/^mode = store/eager = 1/'
red "duplicate collection" \
	's/^\[collection billing\]/[collection th]/'
red "unknown key" \
	's/^workers = 4/workres = 4/'
red "unknown section" \
	's/^\[memory\]/[memroy]/'
red "no listeners" \
	'/^\[listen\]/,/^$/d'
red "no collections" \
	'/^\[collection/,$d'
red "bad port" \
	's/^tcp = 0.0.0.0:6479/tcp = 0.0.0.0:70000/'
red "buckets_log2 out of range" \
	's/^buckets_log2 = 16/buckets_log2 = 30/'
red "duplicate client secret" \
	's/^client = second-app-secret/client = "app secret with spaces and a # inside"/'
red "missing buckets_log2" \
	's/^buckets_log2 = 12//'
red "relative unix path" \
	's|^unix = /run/perfcached.sock|unix = run/perfcached.sock|'
red "unterminated quote" \
	's/^client = "app secret with spaces and a # inside"/client = "unterminated/'
red "no cluster secret" \
	's/^cluster = .*//'
red "key outside any section" <<'EOF'
workers = 4
EOF

echo "configtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
