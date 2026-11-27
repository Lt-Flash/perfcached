#!/bin/bash
# clbench.sh - a three-node perfcached fleet from the clustered Debian package (perfcached), driven through
# the native protocol by natbench (bench/natbench.c) from three client hosts, each key routed to its owner.
#   node 1 = SERVER1 NUMA 0, node 2 = SERVER1 NUMA 1, node 3 = SERVER2 NUMA NODE3_NUMA (1); 12 workers each,
#   unicast discovery between NODE1_IP..NODE3_IP (addresses on those hosts, one per node), MODE eager or
#   shard, persistence off.  Clients: client 1 = CLIENT1 (default SERVER2) bound to NUMA CLIENT1_NUMA (0),
#   128 connections, 16 threads; client 2 = CLIENT2, 112, 14; client 3 = this host (NB3), 112, 14.  Native
#   door: Noise handshake, then plaintext (the default); depth 32; KEYS keys of VAL bytes.
# Each cell (SET, then GET) runs SECS s; rate = the native verbs in the three nodes' INFO commandstats (their
# RESP doors) over WIN s, VALID only if every client is alive at both ends; CPU of the nodes and clients
# over 20 s in the cell (cpu-<cell>-<host>.txt, with NIC rates).  Then each node's stats, dbsize per node,
# and the logs' error/warn lines.
# Variables (required): SERVER1, SERVER2, CLIENT2 = ssh targets; NODE1_IP, NODE2_IP (on SERVER1),
#   NODE3_IP (on SERVER2) = node addresses, up before the run; RESP_ALLOW = network in CIDR allowed on the
#   RESP doors (it must cover the node addresses: the counts are read from SERVER1).
#   Optional: CLIENT1 (SERVER2), CLIENT1_NUMA (0), NODE3_NUMA (1), VAL (200), SECS (40), SSH_USER (root),
#   NB (natbench: its path on CLIENT1 and CLIENT2, named natbench), NB3 (./natbench, on this host), C3COMM (natbench: NB3's
#   process name, set it when NB3 is a wrapper), OUT (./results: results go to $OUT/<tag>/ here),
#   WORK (/var/tmp/sabench on the servers), LOCAL_ALIAS + LOCAL_DEV (an address/prefix this host adds to
#   that device for the run and removes after, when it needs one to reach the node addresses).
# Requires: the perfcached package (perfcached, perfcli), numactl, python3 and redis-cli on both servers;
#   natbench built against libperfd (bench/natbench.c) on the clients; numactl on CLIENT1; python3 on every
#   host; root ssh (BatchMode) to all of them.
# Usage: SERVER1=.. SERVER2=.. CLIENT2=.. NODE1_IP=.. NODE2_IP=.. NODE3_IP=.. RESP_ALLOW=.. [VAL=32|200]
#        bench/clbench.sh <eager|shard> <tag>
set -u
S=sab-client-secret
A=${SERVER1:?set SERVER1=<ssh target of the host for nodes 1 and 2>}
B=${SERVER2:?set SERVER2=<ssh target of the host for node 3>}
C=${CLIENT2:?set CLIENT2=<ssh target of client 2>}
B1=${CLIENT1:-$B} CLIENT1_NUMA=${CLIENT1_NUMA:-0}
N1=${NODE1_IP:?set NODE1_IP=<node 1 address, on SERVER1>}
N2=${NODE2_IP:?set NODE2_IP=<node 2 address, on SERVER1>}
N3=${NODE3_IP:?set NODE3_IP=<node 3 address, on SERVER2>}
RESP_ALLOW=${RESP_ALLOW:?set RESP_ALLOW=<network allowed on the RESP doors, CIDR>}
OUT=${OUT:-./results}; R=${WORK:-/var/tmp/sabench}; SSH_USER=${SSH_USER:-root}
LOCAL_ALIAS=${LOCAL_ALIAS:-} LOCAL_DEV=${LOCAL_DEV:-}
NODES="1:$A:$N1:0:6478 2:$A:$N2:1:6488 3:$B:$N3:${NODE3_NUMA:-1}:6478"
SEEDS=$N1,$N2,$N3
VAL=${VAL:-200} SECS=${SECS:-40} WIN=15 KEYS=1000000 DEPTH=32
NB=${NB:-natbench} NB3=${NB3:-./natbench} C3COMM=${C3COMM:-natbench}
on() { timeout ${T:-120} ssh -o BatchMode=yes -o ConnectTimeout=8 $SSH_USER@$1 "${@:2}"; }
ts() { date '+%T'; }
say() { echo "[$(ts)] $*" | tee -a $O/log.txt; }

MODE=${1:-} TAG=${2:-}
case $MODE in eager|shard) ;; *) echo "usage: clbench.sh <eager|shard> <tag>"; exit 1 ;; esac
O=$OUT/$TAG
[ -n "$TAG" ] && [ ! -e "$O" ] || { echo "ABORT: tag empty or $O exists"; exit 1; }
for h in $A $B $B1 $C; do
	on $h 'pgrep -x perfcached || pgrep -x valkey-server || pgrep -x dragonfly || pgrep -x redis-benchmark || pgrep -x natbench' >/dev/null &&
		{ echo "ABORT: $h busy"; exit 1; }
done
pgrep -x "$C3COMM" >/dev/null && { echo "ABORT: this host (client 3) busy"; exit 1; }
on $A "ip -4 -o addr show | grep -qF ' $N2/'" || { echo "ABORT: node addresses down ($N2 not on SERVER1)"; exit 1; }
mkdir -p $O
if [ -n "$LOCAL_ALIAS" ]; then
	ip -4 addr show dev "$LOCAL_DEV" | grep -qF " $LOCAL_ALIAS" || ip addr add "$LOCAL_ALIAS" dev "$LOCAL_DEV" || { say "ABORT: local alias"; exit 1; }
fi

stop() {
	for h in $A $B; do
		on $h "for f in \$(ls $R/$TAG/*.pid 2>/dev/null); do p=\$(cat \$f); [ \"\$(cat /proc/\$p/comm 2>/dev/null)\" = perfcached ] && kill \$p; done
			for t in \$(seq 1 50); do pgrep -x perfcached >/dev/null || break; sleep 0.2; done
			for f in \$(ls $R/$TAG/*.pid 2>/dev/null); do p=\$(cat \$f); [ \"\$(cat /proc/\$p/comm 2>/dev/null)\" = perfcached ] && kill -9 \$p; done; true"
	done
	for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"; on $h "cat $R/$TAG/log-n$i.txt 2>/dev/null" > $O/log-n$i.txt; done
	for h in $A $B; do on $h "rm -rf $R/$TAG"; done
	[ -n "$LOCAL_ALIAS" ] && ip addr del "$LOCAL_ALIAS" dev "$LOCAL_DEV" 2>/dev/null; true
}
trap stop EXIT

for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"
	printf '[daemon]\nworkers = 12\n[memory]\narena_mb = 4096\n[secrets]\nclient = %s\ncluster = sab-cluster-secret\n[listen]\ntcp = %s:6479\ntcp = 127.0.0.1:%s\nplaintext = loopback\nresp = %s:6380\nresp_allow = %s\n[cluster]\ndiscovery = unicast\nport = 7301\nmax_datagram = 65000\nseeds = %s\nadvertise = %s\nmode = %s\ncollections = 0\n[collection 0]\nbuckets_log2 = 9\nautoscale = auto\ndurable = no\n' \
		"$S" "$ip" "$ap" "$ip" "$RESP_ALLOW" "$SEEDS" "$ip" "$MODE" > $O/n$i.conf
	on $h "mkdir -p $R/$TAG && cat > $R/$TAG/n$i.conf" < $O/n$i.conf || exit 1
	on $h "perfcached -C -f $R/$TAG/n$i.conf > $R/$TAG/check-n$i.txt 2>&1" || { say "ABORT: n$i config refused: $(on $h "tail -3 $R/$TAG/check-n$i.txt")"; exit 1; }
	on $h "cd $R/$TAG; setsid nohup numactl --cpunodebind=$numa --membind=$numa perfcached -f $R/$TAG/n$i.conf -D > $R/$TAG/log-n$i.txt 2>&1 < /dev/null & echo \$! > $R/$TAG/n$i.pid"
done
st() { on $1 "perfcli -h 127.0.0.1 -p $2 -q -j '{\"method\":\"stats\"}'" 2>/dev/null; }
for t in $(seq 1 120); do
	up=0
	for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"
		st $h $ap | python3 -c 'import json,sys
s=json.load(sys.stdin); c=s.get("cluster") or {}; sys.exit(0 if s.get("state")=="ready" and c.get("peers_up")==2 else 1)' 2>/dev/null && up=$((up+1))
	done
	[ $up = 3 ] && break; sleep 0.5
done
[ $up = 3 ] || { say "ABORT: fleet did not converge"; exit 1; }
for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"
	P=$(on $h "cat $R/$TAG/n$i.pid"); on $h "echo n$i: \$(tr '\0' ' ' < /proc/$P/cmdline) \$(readlink /proc/$P/exe) \$(dpkg -S \$(readlink /proc/$P/exe)) \$(grep Cpus_allowed_list /proc/$P/status)"
done > $O/server-live.txt
say "perfcached $MODE, 3 nodes: $(on $A 'perfcached -V | head -1') (package $(on $A "dpkg-query -W -f='\${Package} \${Version}' perfcached")), 12 workers a node; client natbench, libperfd $(strings $NB3 | grep -m1 -xE '0\.7\.[0-9.]+'); converged, waiting 20 s"
sleep 20

CPUPY=$(cat <<'PY'
import os, time
time.sleep(5)
W = 20.0; CLK = os.sysconf('SC_CLK_TCK'); WANT = tuple(dict.fromkeys(('perfcached', 'natbench', '@C3COMM@')))
def snap():
    out = {}
    for p in os.listdir('/proc'):
        if not p.isdigit():
            continue
        try:
            comm = open(f'/proc/{p}/comm').read().strip()
            if comm not in WANT:
                continue
            for t in os.listdir(f'/proc/{p}/task'):
                s = open(f'/proc/{p}/task/{t}/stat').read()
                f = s.rsplit(')', 1)[1].split()
                out[(int(p), int(t))] = (comm, s[s.index('(') + 1:s.rindex(')')], int(f[11]) + int(f[12]))
        except Exception:
            pass
    return out
def busy():
    v = list(map(int, open('/proc/stat').readline().split()[1:])); return sum(v), v[3] + v[4]
def nic():
    out = {}
    for n in os.listdir('/sys/class/net'):
        if n == 'lo' or n.startswith(('bond', 'veth', 'podman', 'cni', 'virbr', 'docker')) or '.' in n:
            continue
        try:
            out[n] = (int(open(f'/sys/class/net/{n}/statistics/rx_bytes').read()), int(open(f'/sys/class/net/{n}/statistics/tx_bytes').read()))
        except Exception:
            pass
    return out
a = snap(); b0 = busy(); n0 = nic(); t0 = time.time(); time.sleep(W); b = snap(); b1 = busy(); n1 = nic(); dt = time.time() - t0
rows = {k: (c, n, 100.0 * (v - a[k][2]) / CLK / dt) for k, (c, n, v) in b.items() if k in a}
tot, idle = b1[0] - b0[0], b1[1] - b0[1]
print('host %s busy %.1f%% of %d cpus over %.1fs' % (os.uname()[1], 100.0 * (tot - idle) / tot, os.cpu_count(), dt))
for n in sorted(n1):
    if n in n0:
        rx, tx = (n1[n][0] - n0[n][0]) * 8 / dt / 1e9, (n1[n][1] - n0[n][1]) * 8 / dt / 1e9
        if rx + tx > 0.05:
            print('  nic %s: rx %.2f Gbit/s, tx %.2f Gbit/s' % (n, rx, tx))
for comm in WANT:
    procs = {}
    for k, (c, n, u) in rows.items():
        if c == comm:
            procs.setdefault(k[0], []).append((u, n))
    if not procs:
        continue
    sums = sorted(((sum(u for u, _ in th), p) for p, th in procs.items()), reverse=True)
    print('  %s x%d: total %.1f%%' % (comm, len(sums), sum(s for s, _ in sums)))
    if comm == 'perfcached':
        for s, p in sums:
            th = sorted(procs[p], reverse=True)
            print('    pid %d %.1f%%: %s' % (p, s, ' '.join('%s=%.1f' % (n, u) for u, n in th if u >= 1.0)))
PY
)
CPUPY=${CPUPY//@C3COMM@/$C3COMM}
sample() {
	on $A "python3 -" <<< "$CPUPY" > $O/cpu-$1-server1.txt 2>&1 &
	on $B "python3 -" <<< "$CPUPY" > $O/cpu-$1-server2.txt 2>&1 &
	[ "$B1" = "$B" ] || on $B1 "python3 -" <<< "$CPUPY" > $O/cpu-$1-c1.txt 2>&1 &
	on $C "python3 -" <<< "$CPUPY" > $O/cpu-$1-c2.txt 2>&1 &
	python3 - <<< "$CPUPY" > $O/cpu-$1-c3.txt 2>&1 &
}
# server count: native verbs in INFO commandstats on the three RESP doors
CNTSH='for ip in NODEIPS; do redis-cli -h $ip -p 6380 info commandstats | tr -d "\r"; done > /tmp/sab-cs.$$; grep -E "^cmdstat_bin:(get|set|mget|mset)[:,]" /tmp/sab-cs.$$ | sed "s/^[^:]*:[^:]*:calls=\([0-9]*\),.*/\1/;s/^[^:]*:calls=\([0-9]*\),.*/\1/" | awk "{s+=\$1} END{print s+0}"; rm -f /tmp/sab-cs.$$'
CNTSH=${CNTSH//NODEIPS/$N1 $N2 $N3}
cell() { # cell <name> <getpct>
	local g=$2
	T=$((SECS + 90)) on $B1 "numactl --cpunodebind=$CLIENT1_NUMA --membind=$CLIENT1_NUMA $NB $N1 6479 $S 0 128 16 $DEPTH $SECS $KEYS $VAL $g 1 50 -1 1" > $O/nb-$1-c1.txt 2>&1 &
	local p1=$!
	T=$((SECS + 90)) on $C "$NB $N1 6479 $S 0 112 14 $DEPTH $SECS $KEYS $VAL $g 1 50 -1 1" > $O/nb-$1-c2.txt 2>&1 &
	local p2=$!
	timeout $((SECS + 90)) $NB3 $N1 6479 $S 0 112 14 $DEPTH $SECS $KEYS $VAL $g 1 50 -1 1 > $O/nb-$1-c3.txt 2>&1 &
	local p3=$!
	sample $1
	sleep 10
	local a1 a2 a3; a1=$(on $B1 'pgrep -xc natbench'); a2=$(on $C 'pgrep -xc natbench'); a3=$(pgrep -xc "$C3COMM")
	local c0 t0 c1 t1
	c0=$(on $A "bash -c '$CNTSH'"); t0=$(date +%s.%N)
	sleep $WIN
	c1=$(on $A "bash -c '$CNTSH'"); t1=$(date +%s.%N)
	local e1 e2 e3; e1=$(on $B1 'pgrep -xc natbench'); e2=$(on $C 'pgrep -xc natbench'); e3=$(pgrep -xc "$C3COMM")
	wait $p1; wait $p2; wait $p3; wait
	python3 - "$c0" "$c1" "$t0" "$t1" "$a1$a2$a3" "$e1$e2$e3" > $O/win-$1.txt <<'PY'
import sys
c0, c1, t0, t1 = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
ok = sys.argv[5] == '111' and sys.argv[6] == '111'
print('%.0f %s clients alive start/end (c1 c2 c3): %s/%s; %d in %.2f s' % ((c1 - c0) / (t1 - t0), 'VALID' if ok else 'INVALID', sys.argv[5], sys.argv[6], c1 - c0, t1 - t0))
PY
	say "== $1: servers answered $(cat $O/win-$1.txt)"
	say "   clients: c1 $(grep -h 'ops/s' $O/nb-$1-c1.txt | head -1) | c2 $(grep -h 'ops/s' $O/nb-$1-c2.txt | head -1) | c3 $(grep -h 'ops/s' $O/nb-$1-c3.txt | head -1)"
	on $A "for ip in $N1 $N2 $N3; do echo \"# \$ip\"; redis-cli -h \$ip -p 6380 info commandstats | tr -d '\r' | grep -E 'calls=[1-9]'; done" > $O/cmdstats-$1.txt
}
cell set 0
cell get 100
for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"; st $h $ap > $O/stats-n$i.json; done
say "keys per node (dbsize): $(on $A "for ip in $N1 $N2 $N3; do redis-cli -h \$ip -p 6380 dbsize; done" | tr '\n' ' ')"
for spec in $NODES; do IFS=: read -r i h ip numa ap <<< "$spec"; on $h "cat $R/$TAG/log-n$i.txt" > $O/log-n$i.txt; done
say "server logs: $(for i in 1 2 3; do echo "n$i $(grep -ciE 'error|crit|fatal' $O/log-n$i.txt) err/crit, $(grep -ciE 'warn' $O/log-n$i.txt) warn;"; done | tr '\n' ' ')"
say "DONE $TAG"
