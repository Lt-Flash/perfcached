#!/bin/bash
# om10rig.sh - an idle eager fleet's background work against its keyspace (1M, then 4M keys, idle 60 s each):
# CPU by thread and the sweep's figures.  Run as: ssh root@<host> bash -s < om10rig.sh
set -u
R=/tmp/om10
mkdir -p $R && cd $R || exit 1
cp /tmp/om3/perfcached $R/ 2>/dev/null || true
PIDS=
cleanup() {
	for p in $PIDS; do kill $p 2>/dev/null; done
	sleep 2
	echo "left: $(pgrep -c -x perfcached) perfcached"
	cd /; rm -rf $R /tmp/om3
}
trap cleanup EXIT
for i in 1 2 3; do
	cat > $R/n$i.conf <<C
[daemon]
workers = 4
log_level = notice
state_dir = $R/s$i
[memory]
arena_mb = 4096
[secrets]
client = om10-client-secret
cluster = om10-cluster-secret
[listen]
tcp = 127.0.181.$i:3041$i
resp = 127.0.181.$i:3042$i
http = 127.0.181.$i:3043$i
plaintext = loopback
[cluster]
multicast = 239.255.181.1:30480
advertise = 127.0.181.$i
mode = eager
collections = 0
[collection 0]
buckets_log2 = 22
C
	chmod 600 $R/n$i.conf; mkdir -p $R/s$i
	taskset -c $(( (i - 1) * 3 ))-$(( (i - 1) * 3 + 2 )) $R/perfcached -f $R/n$i.conf > $R/n$i.log 2>&1 &
	PIDS="$PIDS $!"
done
for i in 1 2 3; do k=0; until grep -q "perfcached ready" $R/n$i.log || [ $k -ge 300 ]; do sleep 0.1; k=$((k+1)); done; done
sleep 5
st() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.181.%s:3043%s/stats" % (sys.argv[1], sys.argv[1]), timeout=5).read())
c=s["cluster"]; e=sum(x.get("entries") or 0 for x in s["collections"])
print("entries=%d peers=%s sweep_ms=%s scanned=%s sent=%s" % (e, c.get("peers_up"), c.get("repl_sweep_ms"),
      c.get("repl_sweep_scanned"), c.get("repl_sweep_sent")))' $1 2>&1; }
fill() { # fill <from> <to>: SET k<i> through node 1's RESP door, pipelined
	python3 - $1 $2 <<'PY'
import socket, sys
a, b = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(("127.0.181.1", 30421), 30); f = s.makefile("rb")
for lo in range(a, b, 2000):
    hi = min(b, lo + 2000)
    s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nk%d\r\n$32\r\n%032d\r\n" % (len(b"k%d" % i), i, i) for i in range(lo, hi)))
    for _ in range(lo, hi):
        f.readline()
PY
}
census() { # census <label>
	declare -A A
	for i in 1 2 3; do
		p=$(echo $PIDS | cut -d' ' -f$i)
		for t in /proc/$p/task/*; do echo "$i $(cat $t/comm) $(cut -d' ' -f14,15 $t/stat)"; done > $R/c$i.a
	done
	sleep 60
	for i in 1 2 3; do
		p=$(echo $PIDS | cut -d' ' -f$i)
		for t in /proc/$p/task/*; do echo "$i $(cat $t/comm) $(cut -d' ' -f14,15 $t/stat)"; done > $R/c$i.b
		echo "-- $1 node $i: $(st $i)"
		awk 'NR==FNR {a[$2]+=$3+$4; next} {b[$2]+=$3+$4} END {for (n in b) { d=(b[n]-a[n])/100; if (d > 0) printf "   %-12s %6.2f s\n", n, d; t+=d } printf "   %-12s %6.2f s in 60 s\n", "all", t}' $R/c$i.a $R/c$i.b | sort -k2 -rn
	done
}
echo "== fleet: $(st 1) | $(st 2) | $(st 3)"
fill 0 1000000; sleep 20
echo "== after 1M: $(st 1) | $(st 2) | $(st 3)"
census "1M idle"
fill 1000000 4000000; sleep 30
echo "== after 4M: $(st 1) | $(st 2) | $(st 3)"
census "4M idle"
echo "== logs: $(cat $R/n*.log | grep -c -E 'ERROR|CRIT') ERROR/CRIT"
