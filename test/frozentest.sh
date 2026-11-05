#!/bin/sh
# frozentest.sh - S476: an eager fleet of three, one node frozen by SIGSTOP under live and new handles; test/frozentest.c
# Node 3 is on the second secret of a rotation: a standby's dial must walk the list as a handle's does.
# Usage: test/frozentest.sh [./perfcached] [./frozentest]
set -u
BIN=${1:-./perfcached}
FT=${2:-./frozentest}
D=$(mktemp -d /var/tmp/pcfrz.XXXXXX)
P1= P2= P3=
trap 'for p in $P1 $P2 $P3; do kill -CONT $p 2>/dev/null; kill -9 $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=frz-client-secret
SEC3=frz-client-secret-rotated
for p in 19681 19691 19692 19693 19699; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "frozentest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	mkdir -p "$D/s$i" "$D/w$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = $([ "$i" = 3 ] && echo "$SEC3" || echo "$SEC")
cluster = frz-cluster-secret
[listen]
tcp = 127.0.166.$i:19681
http = 127.0.166.$i:1969$i
plaintext = never
[cluster]
discovery = unicast
port = 19699
seeds = 127.0.166.1
advertise = 127.0.166.$i
mode = eager
collections = c
[collection c]
buckets_log2 = 10
[wal]
dir = $D/w$i
probe = no
fsync = always
C
	chmod 600 "$D/n$i.conf"
	"$BIN" -f "$D/n$i.conf" >> "$D/n$i.log" 2>&1 &
	eval "P$i=\$!"
	sleep 0.3
done
st() { # st <i> <python expr over r, the /stats document>
	python3 -c 'import json,sys,urllib.request
r=json.load(urllib.request.urlopen("http://127.0.166.%s:1969%s/stats" % (sys.argv[1], sys.argv[1]),timeout=3))
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
k=0; ready=0
while [ $k -lt 300 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
	[ "$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')$(st 3 'r["cluster"]["peers_up"]')" = 222 ] && { ready=1; break; }
	sleep 0.2; k=$((k+1))
done
[ $ready = 1 ] || { echo "  FAIL the fleet did not form: $(tail -2 "$D/n1.log")"; exit 1; }
"$FT" 127.0.166.1 127.0.166.3 19681 "$SEC" "$SEC3" "$P3"
