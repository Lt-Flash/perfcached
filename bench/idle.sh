#!/bin/bash
# idle.sh - each server, started as sabench.sh starts it (one NUMA node of SERVER, THR threads, persistence
# off), with no client for 10 s: does it burn CPU while idle?  If not, 100% under load is work, not spinning.
# One server at a time: perfcached (from PC_CONF), Valkey (io-threads THR), Dragonfly (proactor threads THR).
# Output: per-thread CPU of each, appended to $OUT/idle.txt here.
# Variables (required): SERVER = ssh target; PC_CONF = a perfcached config file here (the server.conf that
#   sabench.sh wrote into its results).  Optional: SERVER_IP (SERVER), PORT (6380), NUMA (1), THR (12),
#   SSH_USER (root), OUT (./results), WORK (/var/tmp/sabench on SERVER).
# Requires: the three packages, numactl and python3 on SERVER; root ssh (BatchMode).
# Usage: SERVER=.. PC_CONF=results/<tag>/server.conf bench/idle.sh
set -u
A=${SERVER:?set SERVER=<ssh target of the server host>}
PC_CONF=${PC_CONF:?set PC_CONF=<a perfcached config, e.g. the server.conf sabench.sh wrote>}
OUT=${OUT:-./results}; R=${WORK:-/var/tmp/sabench}; SSH_USER=${SSH_USER:-root}
IP=${SERVER_IP:-$A} PORT=${PORT:-6380} NUMA=${NUMA:-1} THR=${THR:-12}
mkdir -p "$OUT"
on() { timeout ${T:-60} ssh -o BatchMode=yes -o ConnectTimeout=8 $SSH_USER@$1 "${@:2}"; }
on $A 'pgrep -x perfcached || pgrep -x valkey-server || pgrep -x dragonfly' >/dev/null && { echo "ABORT: $A busy"; exit 1; }
for kind in pc valkey dfly; do
	D=$R/idle-$kind; on $A "rm -rf $D; mkdir -p $D"
	case $kind in
	pc) on $A "cat > $D/server.conf" < "$PC_CONF"; CMD="perfcached -f $D/server.conf -D" ;;
	valkey) CMD="valkey-server --bind $IP --port $PORT --io-threads $THR --save '' --appendonly no --protected-mode no --daemonize no --dir $D --logfile ''" ;;
	dfly) CMD="dragonfly --bind $IP --port $PORT --proactor_threads $THR --dbfilename '' --dir $D --logtostderr" ;;
	esac
	on $A "cd $D; setsid nohup numactl --cpunodebind=$NUMA --membind=$NUMA $CMD > $D/server.log 2>&1 < /dev/null & echo \$! > $D/server.pid"
	sleep 6
	P=$(on $A "cat $D/server.pid")
	on $A "python3 - $P" <<'PY' | sed "s/^/$kind: /" >> "$OUT/idle.txt"
import os, sys, time
p = sys.argv[1]; CLK = os.sysconf('SC_CLK_TCK')
def snap():
    out = {}
    for t in os.listdir(f'/proc/{p}/task'):
        s = open(f'/proc/{p}/task/{t}/stat').read(); f = s.rsplit(')', 1)[1].split()
        out[t] = (s[s.index('(') + 1:s.rindex(')')], int(f[11]) + int(f[12]))
    return out
a = snap(); time.sleep(10); b = snap()
rows = sorted(((100.0 * (b[t][1] - a[t][1]) / CLK / 10, b[t][0]) for t in b if t in a), reverse=True)
print('idle 10 s, %d threads, total %.1f%% CPU; busiest: %s' % (len(rows), sum(r[0] for r in rows), ' '.join('%s=%.1f' % (n, u) for u, n in rows[:6])))
PY
	on $A "p=\$(cat $D/server.pid); c=\$(cat /proc/\$p/comm); case \"\$c\" in perfcached|valkey-server|dragonfly) kill \$p ;; esac
		for t in \$(seq 1 50); do [ -d /proc/\$p ] || break; sleep 0.2; done; [ -d /proc/\$p ] && kill -9 \$p; rm -rf $D"
done
cat "$OUT/idle.txt"
