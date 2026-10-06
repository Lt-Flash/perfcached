#!/bin/sh
# difftest.sh - S317's gate (DESIGN 12if): the 0.4.5 daemon, which speaks
# the binary protocol only, answers every native method the same as the
# last daemon with the JSON-RPC door did.  Two single-node daemons from
# ONE config, the reference on JSON lines and the new one on CMD frames,
# one corpus of requests (test/difftest.py), replies compared as decoded
# values after the documented shape changes are normalised.
#
# Fail-first: against the reference binary on BOTH sides the corpus
# matches trivially; against a 0.4.5 daemon that still differs anywhere
# a line names the method and both replies.
#
# Usage: test/difftest.sh <new-perfcached> <reference-perfcached>
set -u
NEW=${1:-./perfcached}
REF=${2:-${PC_REF:-/var/tmp/perfcached-044-ref}}
# The reference is a build of the last tree with the JSON door (p044,
# 01f3180), pinned OUTSIDE this tree: on the GitLab runner host it lives
# at /var/tmp/perfcached-044-ref; a runner without one (GitHub) cannot
# run the gate and says so rather than passing it.
if [ ! -x "$REF" ]; then
	echo "difftest: SKIP - no reference daemon at $REF (PC_REF=<path> to point at one) - nothing was tested"
	exit 0
fi
for p in 18701 18702 18711 18712; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "difftest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcdiff.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM

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
