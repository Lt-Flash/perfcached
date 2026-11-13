#!/bin/sh
# quietlinktest.sh - four eager nodes: quiet links closed by a node's death, a routed add to a frozen node; test/quietlinktest.c
# Usage: test/quietlinktest.sh [./perfcached] [./quietlinktest]
set -u
BIN=${1:-./perfcached}
QT=${2:-./quietlinktest}
D=$(mktemp -d /var/tmp/pcqlt.XXXXXX)
P1= P2= P3= P4=
trap 'for p in $P1 $P2 $P3 $P4; do kill -9 $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
SEC=qlt-client-secret
for p in 19970 19971 19972 19973 19974 19979; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "quietlinktest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3 4; do
	mkdir -p "$D/s$i" "$D/w$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = qlt-cluster-secret
[listen]
tcp = 127.0.167.$i:19970
http = 127.0.167.$i:1997$i
plaintext = never
[cluster]
discovery = unicast
port = 19979
seeds = 127.0.167.1
advertise = 127.0.167.$i
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
r=json.load(urllib.request.urlopen("http://127.0.167.%s:1997%s/stats" % (sys.argv[1], sys.argv[1]),timeout=3))
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
k=0; ready=0
while [ $k -lt 300 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')$(st 4 'r["state"]')" = readyreadyreadyready ] && \
	[ "$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')$(st 3 'r["cluster"]["peers_up"]')$(st 4 'r["cluster"]["peers_up"]')" = 3333 ] && { ready=1; break; }
	sleep 0.2; k=$((k+1))
done
[ $ready = 1 ] || { echo "  FAIL the fleet did not form: $(tail -2 "$D/n1.log")"; exit 1; }
"$QT" 127.0.167.1 127.0.167.2 127.0.167.3 127.0.167.4 19970 "$SEC" "$P1" "$P3" "$P4"
rc=$?
if [ $rc != 0 ]; then
	for i in 2 3; do
		echo "  ..   node $i: master $(st $i 'r["cluster"].get("master")') peers_up $(st $i 'r["cluster"]["peers_up"]')"
		echo "         $(st $i '" ".join("%s=%s" % (k, v) for k, v in sorted(r["cluster"].items()) if v and isinstance(v, int) and any(w in k for w in ("repl_", "replay", "drop", "refused", "lost", "stale")) and not k.startswith("win_"))')"
		echo "         its log's last cluster lines:"
		grep -E 'cluster|repl|push|sweep' "$D/n$i.log" | tail -12 | cut -c1-200 | sed 's/^/         /'
	done
fi
exit $rc
