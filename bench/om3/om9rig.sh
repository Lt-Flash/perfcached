#!/bin/bash
# om9rig.sh - bytes on the wire per replicated write: 1M SETs through one node's RESP door, alone and in a three-node
# eager fleet on loopback, lo's counters around each fill.  Run as: ssh root@<host> bash -s < om9rig.sh
set -u
R=/tmp/om9
mkdir -p $R && cd $R || exit 1
cp /tmp/om3/perfcached $R/
PIDS=
cleanup() { for p in $PIDS; do kill $p 2>/dev/null; done; sleep 3; echo "left: $(pgrep -c -x perfcached) perfcached"; cd /; rm -rf $R /tmp/om3; }
trap cleanup EXIT
conf() { # conf <i> <cluster 0|1>
	cat > $R/n$1.conf <<C
[daemon]
workers = 4
log_level = notice
state_dir = $R/s$1
[memory]
arena_mb = 2048
[secrets]
client = om9-client-secret
cluster = om9-cluster-secret
[listen]
tcp = 127.0.182.$1:3051$1
resp = 127.0.182.$1:3052$1
http = 127.0.182.$1:3053$1
plaintext = loopback
C
	[ "$2" = 1 ] && printf '[cluster]\nmulticast = 239.255.182.1:30580\nadvertise = 127.0.182.%s\nmode = eager\ncollections = 0\n' $1 >> $R/n$1.conf
	printf '[collection 0]\nbuckets_log2 = 21\n' >> $R/n$1.conf
	chmod 600 $R/n$1.conf; mkdir -p $R/s$1
}
start() { # start <i>
	taskset -c $(( ($1 - 1) * 3 ))-$(( ($1 - 1) * 3 + 2 )) $R/perfcached -f $R/n$1.conf > $R/n$1.log 2>&1 &
	PIDS="$PIDS $!"
	k=0; until grep -q "perfcached ready" $R/n$1.log || [ $k -ge 300 ]; do sleep 0.1; k=$((k+1)); done
}
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.182.%s:3053%s/stats" % (sys.argv[1], sys.argv[1]), timeout=5).read())
c=s.get("cluster") or {}; e=sum(x.get("entries") or 0 for x in s["collections"])
print("entries=%d repl_pushed=%s repl_groups=%s" % (e, c.get("repl_pushed"), c.get("repl_groups")))' $1 2>&1; }
fill() {
	python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.182.1", 30521), 30); f = s.makefile("rb")
for lo in range(0, 1000000, 2000):
    s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nk%d\r\n$32\r\n%032d\r\n" % (len(b"k%d" % i), i, i) for i in range(lo, lo + 2000)))
    for _ in range(2000):
        f.readline()
PY
}
lo() { echo "$(cat /sys/class/net/lo/statistics/tx_bytes) $(cat /sys/class/net/lo/statistics/tx_packets)"; }
measure() { # measure <label> <settle s>
	set -- "$1" "$2" $(lo)
	fill; sleep "$2"
	set -- "$1" "$2" "$3" "$4" $(lo)
	echo "$1 $(( $5 - $3 )) $(( $6 - $4 ))"
}
conf 1 0; start 1
A=$(measure lone 2)
echo "== lone: $(st 1)"
kill $PIDS; wait 2>/dev/null; PIDS=; rm -rf $R/s1
for i in 1 2 3; do conf $i 1; done; for i in 1 2 3; do start $i; done; sleep 8
B=$(measure fleet 15)
echo "== fleet: $(st 1) | $(st 2) | $(st 3)"
set -- $A $B
awk -v lb=$2 -v lp=$3 -v fb=$5 -v fp=$6 'BEGIN {
	printf "lone fill:  %d bytes, %d packets on lo\n", lb, lp
	printf "fleet fill: %d bytes, %d packets on lo\n", fb, fp
	printf "replication: %.1f bytes and %.3f packets a copy (2,000,000 copies); payload ~40 B (key + value)\n", (fb - lb) / 2e6, (fp - lp) / 2e6 }'
echo "== logs: $(cat $R/n*.log | grep -c -E 'ERROR|CRIT') ERROR/CRIT"
