#!/bin/sh
# phpexttest.sh - PE-1: the PHP extension end to end against live daemons, a
# plaintext loopback door and a door that takes Noise only: test/phpexttest.php
# drives every verb, and reads the daemon's native.plain.conns: the default stays
# encrypted past the handshake, and encrypt => false does not.
# Usage: test/phpexttest.sh [./perfcached] [php8.3] [lang/php/modules/perfcached.so]
set -u
BIN=${1:-./perfcached}
PHP=${2:-php8.3}
EXT=${3:-lang/php/modules/perfcached.so}
case $EXT in /*) ;; *) EXT=$(pwd)/$EXT ;; esac
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcphp.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$EXT" ] || { echo "phpexttest: no extension at $EXT"; exit 1; }
for p in 17155 17156; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "phpexttest: port $p already bound" >&2; exit 1; }
done
mkconf() { # mkconf <file> <port> <plaintext>
	cat > "$1" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = php-test-secret
cluster = php-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = $3
[collection c]
buckets_log2 = 10
C
	chmod 600 "$1"
}
mkconf "$D/pt.conf" 17155 loopback
mkconf "$D/nx.conf" 17156 never
"$BIN" -f "$D/pt.conf" > "$D/pt.log" 2>&1 &
P1=$!
"$BIN" -f "$D/nx.conf" > "$D/nx.log" 2>&1 &
P2=$!
i=0; while [ $i -lt 150 ]; do
	grep -q "perfcached ready" "$D/pt.log" && grep -q "perfcached ready" "$D/nx.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/nx.log" || { echo "phpexttest: the daemons did not start: $(tail -2 "$D/nx.log" | tr '\n' ' ')"; exit 1; }
PX() { "$PHP" -n -d extension="$EXT" "$@"; }
V=$(PX --ri perfcached 2>&1 | grep -c "perfcached => enabled")
[ "$V" = 1 ] && ok "the extension loads in $("$PHP" -n -r 'echo PHP_VERSION, " ", PHP_ZTS ? "ZTS" : "NTS";')" \
	|| bad "the extension did not load: $(PX --ri perfcached 2>&1 | tail -2 | tr '\n' ' ')"
PX "$T/phpexttest.php" 17155 17156 php-test-secret
PF=$?
[ $PF = 0 ] || fail=$((fail+1))
echo "phpexttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
