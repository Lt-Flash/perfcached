#!/bin/sh
# difftest.sh - the binary-only daemon answers every native method as the last JSON-RPC daemon did, one corpus
# (test/difftest.py) compared as decoded values.  Usage: test/difftest.sh <new-perfcached> <reference-perfcached>
set -u
NEW=${1:-./perfcached}
REF=${2:-${PC_REF:-/var/tmp/perfcached-044-ref}}
# the reference is a build of the last tree with the JSON door, pinned outside this tree; without it
# the gate says so rather than pass, and under GitLab CI fails
if [ ! -x "$REF" ]; then
	if [ -n "${GITLAB_CI:-}" ]; then
		echo "difftest: FAILED - no reference daemon at $REF on GitLab runner $(hostname) - every GitLab runner must carry it (PC_REF=<path>)"
		exit 1
	fi
	echo "difftest: SKIP - no reference daemon at $REF (PC_REF=<path> to point at one) - nothing was tested"
	exit 0
fi
for p in 18701 18702 18711 18712; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "difftest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcdiff.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

conf() { # conf <n>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = dt-client-secret
enable = dt-enable
[listen]
tcp = 127.0.0.1:1870$1
http = 127.0.0.1:1871$1
plaintext = loopback
[collection 0]
buckets_log2 = 10
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <binary>
	: > "$D/n$1.log"
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "difftest: daemon $1 ($2) did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
conf 1; conf 2
start 1 "$REF" || exit 1
start 2 "$NEW" || exit 1
echo "--- reference $("$REF" -V 2>&1 | head -1) vs new $("$NEW" -V 2>&1 | head -1)"
python3 "$(dirname "$0")/difftest.py" 18701 18702
rc=$?
grep -iE 'error|assert|segv|crash' "$D/n1.log" "$D/n2.log" | grep -v 'log_level' | head -5
exit $rc
