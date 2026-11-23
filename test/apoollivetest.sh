#!/bin/sh
# apoollivetest.sh - the async pool on a three-node fleet (test/apoollivetest.c); the client asks
# this shell over a FIFO to stop, continue, kill and start nodes, so no daemon inherits its sockets.
# Usage: test/apoollivetest.sh [./perfcached] [./apoollivetest]
set -u
BIN=${1:-./perfcached}
AT=${2:-./apoollivetest}
D=$(mktemp -d /var/tmp/pcapool.XXXXXX)
CP=
trap 'for i in 1 2 3; do p=$(cat "$D/n$i.pid" 2>/dev/null) && kill -CONT "$p" 2>/dev/null; [ -n "${p:-}" ] && kill -9 "$p" 2>/dev/null; done; [ -n "$CP" ] && kill -9 $CP 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=apool-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19660 19661 19662 19663 19669; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "apoollivetest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = apool-cluster-secret
[listen]
tcp = 127.0.177.$i:19660
http = 127.0.177.$i:1966$i
plaintext = never
[cluster]
discovery = unicast
port = 19669
seeds = 127.0.177.1
advertise = 127.0.177.$i
mode = eager
collections = c
[collection c]
buckets_log2 = 10
C
	chmod 600 "$D/n$i.conf"
done
up() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 < /dev/null & echo $! > "$D/n$1.pid"; }
st() { # st <i>: the node's state, "?" when it does not answer
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.177.%s:1966%s/stats" % (sys.argv[1], sys.argv[1]),timeout=3))
print(r["state"])' "$1" 2>/dev/null || echo "?"
}
listed() { # listed <i>: the members node <i> lists with a client port, ready ones
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.177.%s:1966%s/members" % (sys.argv[1], sys.argv[1]),timeout=3))
print(sum(1 for m in r.get("members") or [] if m.get("port") and m.get("state", "ready") == "ready"))' "$1" 2>/dev/null || echo "?"
}
settled() { # every node ready and listing three ready members
	k=0
	while [ $k -lt 200 ]; do
		[ "$(st 1)$(st 2)$(st 3)" = readyreadyready ] && [ "$(listed 1)$(listed 2)$(listed 3)" = 333 ] && return 0
		sleep 0.1; k=$((k+1))
	done
	return 1
}

up 1; sleep 0.3; up 2; up 3
settled || { echo "  FAIL the fleet did not form: $(tail -2 "$D/n1.log")"; echo "apoollivetest: 0 passed, 1 failed"; exit 1; }
ok "three eager members formed, each listing three ready"

mkfifo "$D/cmd" "$D/ack"
"$AT" "$SEC" "$D/cmd" "$D/ack" 127.0.177.1:19660 127.0.177.2:19660 127.0.177.3:19660 >> "$D/at.out" 2>&1 &
CP=$!
while read -r c i; do
	p=$(cat "$D/n$i.pid" 2>/dev/null)
	case "$c" in
	stop) kill -STOP "$p" ;;
	cont) kill -CONT "$p" ;;
	kill)
		kill -9 "$p"; wait "$p" 2>/dev/null ;;
	start)
		up "$i"
		settled || echo "FAIL: node $i did not come back ready: $(tail -2 "$D/n$i.log")" >> "$D/at.out" ;;
	esac
	timeout 60 sh -c 'echo done > "$1"' _ "$D/ack"
done < "$D/cmd"
wait $CP
RC=$?
CP=
sed -n 's/^ok: /  ok   /p;s/^FAIL: /  FAIL /p;s/^--- /  --- /p' "$D/at.out"
N_OK=$(grep -c "^ok: " "$D/at.out" 2>/dev/null); N_OK=${N_OK:-0}
N_BAD=$(grep -c "^FAIL: " "$D/at.out" 2>/dev/null); N_BAD=${N_BAD:-0}
pass=$((pass + N_OK)); fail=$((fail + N_BAD))
# a client that died before its summary is a failure, not a clean count
grep -q "^apoollivetest: " "$D/at.out" || bad "the client did not finish (exit $RC): $(tail -3 "$D/at.out" | tr '\n' ' ')"
[ $RC -eq 0 ] || [ $N_BAD -gt 0 ] || bad "the client exited $RC with no failure named"
if [ $fail -gt 0 ]; then
	for i in 1 2 3; do
		echo "  --- n$i log tail"
		tail -15 "$D/n$i.log" | cut -c1-200 | sed 's/^/    /'
	done
fi
echo "apoollivetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
