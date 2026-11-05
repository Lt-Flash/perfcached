#!/bin/sh
# phpsymfonytest.sh - PE-3: PerfcachedAdapter under symfony/cache 5.4, the line
# the site runs, used the way its services use a pool: test/phpsymfonytest.php.
# Usage: test/phpsymfonytest.sh [./perfcached] [php8.3] [lang/php/modules/perfcached.so] [autoload.php]
set -u
BIN=${1:-./perfcached}
PHP=${2:-php8.3}
EXT=${3:-lang/php/modules/perfcached.so}
VND=${4:-.phpvendor/vendor/autoload.php}
case $EXT in /*) ;; *) EXT=$(pwd)/$EXT ;; esac
case $VND in /*) ;; *) VND=$(pwd)/$VND ;; esac
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/.." && pwd)
D=$(mktemp -d /var/tmp/pcsym.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PORT=17158
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "phpsymfonytest: port $PORT already bound" >&2; exit 1; }
cat > "$D/d.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = php-sym-secret
cluster = php-sym-cluster
[listen]
tcp = 127.0.0.1:$PORT
plaintext = never
[collection c]
buckets_log2 = 10
C
chmod 600 "$D/d.conf"
"$BIN" -f "$D/d.conf" > "$D/d.log" 2>&1 &
P1=$!
i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/d.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/d.log" || { echo "phpsymfonytest: the daemon did not start"; exit 1; }
[ -f "$VND" ] || { echo "phpsymfonytest: FAILED - no $VND: make phpvendor"; exit 1; }
"$PHP" -d extension="$EXT" -d error_log="$D/php.log" -d log_errors=1 "$T/phpsymfonytest.php" \
	"$VND" "$R/lang/php/symfony/src/PerfcachedAdapter.php" $PORT php-sym-secret "$D/php.log"
