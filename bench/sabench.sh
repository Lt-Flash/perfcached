#!/bin/bash
# sabench.sh - one server, installed from its package, against redis-benchmark from three client hosts, through
# the Redis protocol: perfcached (perfcached-standalone .deb), Valkey (valkey-server) or Dragonfly (its .deb).
# The server runs on one NUMA node of SERVER (numactl --cpunodebind/--membind=$NUMA) with THR threads
# (perfcached workers, Valkey io-threads, Dragonfly proactor threads), persistence off.  Clients:
#   client 1 = CLIENT1 (ssh, redis-benchmark in its PATH): 3 processes x 8 threads x 40 connections
#   client 2 = CLIENT2 (ssh, a shell from CLIENT2_SHELL that has redis-benchmark): 2 x 7 x 60
#   client 3 = this host (RB3; C3COMM = its process name, for the liveness counts): 2 x 7 x 60
# Each cell: a fill (2 x KEYS SETs), a pilot that sizes each client for SECS s, then the measured SET and GET
# cells, pipeline 32, KEYS random keys, VAL-byte values.  Rate = the server's own INFO commandstats SET/GET
# calls over WIN s in the middle of the cell; the cell is VALID only if every client process is alive at
# both ends of that window.  CPU = every thread of the server process and of the clients over 20 s inside
# the cell (cpu-<cell>-<host>.txt, with NIC rates).  Then 60 random keys read back, dbsize, files written
# by the server, and its log's error/warn lines.
# Variables (required): SERVER, CLIENT1, CLIENT2 = ssh targets; RESP_ALLOW = the clients' network in CIDR
#   (perfcached's resp_allow).  Optional: SERVER_IP (address the server binds and clients dial, default
#   SERVER), PORT (6380), NUMA (1), THR (12), VAL (200), SECS (40), SSH_USER (root),
#   OUT (./results: results go to $OUT/<tag>/ here), WORK (/var/tmp/sabench on SERVER),
#   CLIENT2_SHELL ("bash -s"; the published run used
#     "podman run -i --rm --network host --entrypoint sh docker.io/library/redis:8.0.2 -s"),
#   RB3 (redis-benchmark), C3COMM (redis-benchmark; set it when RB3 is a wrapper with another name).
# Requires: the server package installed on SERVER, plus numactl, python3 and redis-cli there;
#   redis-benchmark 8.x on the clients; python3 on every host; root ssh (BatchMode) to all three.
# Usage: SERVER=.. CLIENT1=.. CLIENT2=.. RESP_ALLOW=.. [VAL=32|200] bench/sabench.sh <pc|valkey|dfly> <tag>
set -u
A=${SERVER:?set SERVER=<ssh target of the server host>}
B=${CLIENT1:?set CLIENT1=<ssh target of client 1>}
C=${CLIENT2:?set CLIENT2=<ssh target of client 2>}
RESP_ALLOW=${RESP_ALLOW:?set RESP_ALLOW=<the clients network, CIDR>}
OUT=${OUT:-./results}; R=${WORK:-/var/tmp/sabench}; SSH_USER=${SSH_USER:-root}
RB3=${RB3:-redis-benchmark}; C3COMM=${C3COMM:-redis-benchmark}; CLIENT2_SHELL=${CLIENT2_SHELL:-bash -s}
IP=${SERVER_IP:-$A} PORT=${PORT:-6380} NUMA=${NUMA:-1} THR=${THR:-12}
KEYS=1000000 VAL=${VAL:-200} PIPE=32 SECS=${SECS:-40} WIN=15
on() { timeout ${T:-120} ssh -o BatchMode=yes -o ConnectTimeout=8 $SSH_USER@$1 "${@:2}"; }
ts() { date '+%T'; }
say() { echo "[$(ts)] $*" | tee -a $O/log.txt; }

KIND=${1:-} TAG=${2:-}
case $KIND in pc|valkey|dfly) ;; *) echo "usage: sabench.sh <pc|valkey|dfly> <tag>   (SERVER, CLIENT1, CLIENT2, RESP_ALLOW: see the header)"; exit 1 ;; esac
O=$OUT/$TAG
[ -n "$TAG" ] && [ ! -e "$O" ] || { echo "ABORT: tag empty or $O exists"; exit 1; }
for h in $A $B $C; do
	on $h 'pgrep -x perfcached || pgrep -x valkey-server || pgrep -x dragonfly || pgrep -x redis-server || pgrep -x redis-benchmark || pgrep -x natbench' >/dev/null &&
		{ echo "ABORT: $h busy"; exit 1; }
done
pgrep -x "$C3COMM" >/dev/null && { echo "ABORT: this host (client 3) busy"; exit 1; }
mkdir -p $O

stop() {
	on $A "p=\$(cat $R/$TAG/server.pid 2>/dev/null); c=\$(cat /proc/\$p/comm 2>/dev/null)
		case \"\$c\" in perfcached|valkey-server|dragonfly) kill \$p ;; *) exit 0 ;; esac
		for t in \$(seq 1 50); do [ -d /proc/\$p ] || break; sleep 0.2; done
		[ -d /proc/\$p ] && kill -9 \$p; true"
	on $A "cat $R/$TAG/server.log 2>/dev/null" > $O/server.log
	on $A "rm -rf $R/$TAG"
}
trap stop EXIT

# ---- start the server -----------------------------------------------------------------------------------
on $A "mkdir -p $R/$TAG"
case $KIND in
pc)
	printf '[daemon]\nworkers = %s\n[memory]\narena_mb = 4096\n[secrets]\nclient = sab-client-secret\n[listen]\ntcp = 127.0.0.1:6479\nresp = %s:%s\nresp_allow = %s\n[collection 0]\nbuckets_log2 = 9\nautoscale = auto\ndurable = no\n' $THR $IP $PORT "$RESP_ALLOW" > $O/server.conf
	on $A "cat > $R/$TAG/server.conf" < $O/server.conf
	on $A "perfcached -C -f $R/$TAG/server.conf > $R/$TAG/check.txt 2>&1" || { say "ABORT: config refused: $(on $A "tail -3 $R/$TAG/check.txt")"; exit 1; }
	CMD="perfcached -f $R/$TAG/server.conf -D" ;;
valkey)
	CMD="valkey-server --bind $IP --port $PORT --io-threads $THR --save '' --appendonly no --protected-mode no --daemonize no --dir $R/$TAG --logfile ''" ;;
dfly)
	CMD="dragonfly --bind $IP --port $PORT --proactor_threads $THR --dbfilename '' --dir $R/$TAG --logtostderr" ;;
esac
echo "$CMD" > $O/server.cmd
on $A "cd $R/$TAG; setsid nohup numactl --cpunodebind=$NUMA --membind=$NUMA $CMD > $R/$TAG/server.log 2>&1 < /dev/null & echo \$! > $R/$TAG/server.pid"
for t in $(seq 1 60); do on $A "redis-cli -h $IP -p $PORT ping 2>/dev/null" | grep -q PONG && break; sleep 0.5; done
on $A "redis-cli -h $IP -p $PORT ping" | grep -q PONG || { say "ABORT: server not answering"; exit 1; }
sleep 3
P=$(on $A "cat $R/$TAG/server.pid")
on $A "tr '\0' ' ' < /proc/$P/cmdline; echo; echo comm=\$(cat /proc/$P/comm) threads=\$(ls /proc/$P/task | wc -l);
	grep -E 'Cpus_allowed_list|Mems_allowed_list' /proc/$P/status; readlink /proc/$P/exe; dpkg -S \$(readlink /proc/$P/exe)" > $O/server-live.txt
VER=$(case $KIND in pc) on $A 'perfcached -V | head -1' ;; valkey) on $A 'valkey-server --version' | cut -d' ' -f1-3 ;; dfly) on $A 'dragonfly --version 2>&1 | head -1' | sed 's/\x1b\[[0-9;]*m//g' ;; esac)
PKG=$(on $A "dpkg-query -W -f='\${Package} \${Version}' \$(dpkg -S \$(readlink /proc/$P/exe) | cut -d: -f1)")
say "$KIND: $VER (package $PKG), NUMA $NUMA, $THR threads, live: $(sed -n 2,4p $O/server-live.txt | tr '\n' ' ' | tr -s '\t ' ' ')"
# persistence off, read back
case $KIND in
valkey) say "persistence: save='$(on $A "redis-cli -h $IP -p $PORT config get save | sed -n 2p")' appendonly=$(on $A "redis-cli -h $IP -p $PORT config get appendonly | sed -n 2p") io-threads=$(on $A "redis-cli -h $IP -p $PORT config get io-threads | sed -n 2p")" ;;
dfly) say "persistence: dbfilename='$(on $A "redis-cli -h $IP -p $PORT config get dbfilename | sed -n 2p")'" ;;
pc) say "persistence: no [wal] in the config; $(on $A "grep -ciE 'wal' $R/$TAG/server.log") log lines mention wal" ;;
esac

# ---- CPU sampler (20 s inside a cell) -------------------------------------------------------------------
CPUPY=$(cat <<'PY'
import os, time
time.sleep(5)
W = 20.0; CLK = os.sysconf('SC_CLK_TCK'); WANT = tuple(dict.fromkeys(('perfcached', 'valkey-server', 'dragonfly', 'redis-benchmark', '@C3COMM@')))
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
                out[(int(p), int(t))] = (comm, s[s.index('(') + 1:s.rindex(')')], int(f[11]) + int(f[12]), int(f[36]))
        except Exception:
            pass
    return out
def busy():
    v = list(map(int, open('/proc/stat').readline().split()[1:])); return sum(v), v[3] + v[4]
def soft():
    return [int(l.split()[7]) for l in open('/proc/stat') if l.startswith('cpu') and l[3] != ' ']
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
a = snap(); b0 = busy(); s0 = soft(); n0 = nic(); t0 = time.time(); time.sleep(W); b = snap(); b1 = busy(); s1 = soft(); n1 = nic(); dt = time.time() - t0
rows = {k: (c, n, 100.0 * (v - a[k][2]) / CLK / dt, cpu) for k, (c, n, v, cpu) in b.items() if k in a}
tot, idle = b1[0] - b0[0], b1[1] - b0[1]
print('host %s busy %.1f%% of %d cpus over %.1fs; softirq %.1f cpus' % (os.uname()[1], 100.0 * (tot - idle) / tot, os.cpu_count(), dt, sum(y - x for x, y in zip(s0, s1)) / CLK / dt))
for n in sorted(n1):
    if n in n0:
        rx, tx = (n1[n][0] - n0[n][0]) * 8 / dt / 1e9, (n1[n][1] - n0[n][1]) * 8 / dt / 1e9
        if rx + tx > 0.05:
            print('  nic %s: rx %.2f Gbit/s, tx %.2f Gbit/s' % (n, rx, tx))
for comm in WANT:
    procs = {}
    for k, (c, n, u, cpu) in rows.items():
        if c == comm:
            procs.setdefault(k[0], []).append((u, n, cpu))
    if not procs:
        continue
    sums = sorted(((sum(u for u, _, _ in th), p) for p, th in procs.items()), reverse=True)
    print('  %s x%d: total %.1f%%, busiest process %.1f%%' % (comm, len(sums), sum(s for s, _ in sums), sums[0][0]))
    if comm not in ('redis-benchmark', '@C3COMM@'):
        for s, p in sums:
            th = sorted(procs[p], reverse=True)
            print('    pid %d %.1f%%: %s' % (p, s, ' '.join('%s@%d=%.1f' % (n, cpu, u) for u, n, cpu in th if u >= 1.0)))
PY
)
CPUPY=${CPUPY//@C3COMM@/$C3COMM}
sample() {
	on $A "python3 -" <<< "$CPUPY" > $O/cpu-$1-server.txt 2>&1 &
	on $B "python3 -" <<< "$CPUPY" > $O/cpu-$1-c1.txt 2>&1 &
	on $C "python3 -" <<< "$CPUPY" > $O/cpu-$1-c2.txt 2>&1 &
	python3 - <<< "$CPUPY" > $O/cpu-$1-c3.txt 2>&1 &
}

# ---- clients: NP redis-benchmark processes per host, NT threads each, c connections each ----------------
cli_script() { # cli_script <SET|GET> <n per process> <procs> <threads> <conns>
	local ph=$1 n=$2 np=$3 nt=$4 c=$5 i
	echo "V=\$(printf '%${VAL}s' '' | tr ' ' x); B=\${RB:-redis-benchmark}; D=\$(mktemp -d)"
	for i in $(seq 1 $np); do
		if [ $ph = SET ]; then cmd="SET key:__rand_int__ \"\$V\""; else cmd="GET key:__rand_int__"; fi
		echo "\$B -h $IP -p $PORT -n $n -c $c --threads $nt -P $PIPE -r $KEYS --csv $cmd > \$D/o$i 2>\$D/e$i &"
	done
	echo "wait"
	for i in $(seq 1 $np); do
		echo "echo \"RES $i $n \$(grep -E '^\"(SET|GET)' \$D/o$i | tail -1)\"; [ -s \$D/e$i ] && sed 's/^/ERR $i /' \$D/e$i | head -3"
	done
	echo "rm -rf \$D"
}
NP1=3 NT1=8 C1=40 NP2=2 NT2=7 C2=60 NP3=2 NT3=7 C3=60
drive() { # drive <cell> <SET|GET> <n c1> <n c2> <n c3>
	cli_script $2 $3 $NP1 $NT1 $C1 > $O/cli-$1-c1.sh
	cli_script $2 $4 $NP2 $NT2 $C2 > $O/cli-$1-c2.sh
	cli_script $2 $5 $NP3 $NT3 $C3 > $O/cli-$1-c3.sh
	RB=$RB3 timeout $((SECS * 4 + 120)) bash -s < $O/cli-$1-c3.sh > $O/out-$1-c3.txt 2>&1 &
	local p0=$!
	T=$((SECS * 4 + 120)) on $B "bash -s" < $O/cli-$1-c1.sh > $O/out-$1-c1.txt 2>&1 &
	local p1=$!
	T=$((SECS * 4 + 120)) on $C "$CLIENT2_SHELL" < $O/cli-$1-c2.sh > $O/out-$1-c2.txt 2>&1 &
	local p2=$!
	wait $p0; wait $p1; wait $p2
}
SUMPY=$(cat <<'PY'
import sys, csv
tot_n = 0; tmax = 0.0; rsum = 0.0; p50 = 0.0; p99 = 0.0; errs = 0; k = 0
for fn in sys.argv[1:]:
    for ln in open(fn):
        if ln.startswith('ERR') and 'Could not fetch server CONFIG' not in ln:
            errs += 1
        if not ln.startswith('RES '):
            continue
        _, i, n, rest = ln.split(' ', 3)
        row = next(csv.reader([rest.strip()]))
        if len(row) < 8:
            print('MISSING', fn, i); sys.exit(1)
        n = int(n); rps = float(row[1]); k += 1
        tot_n += n; tmax = max(tmax, n / rps); rsum += rps
        p50 = max(p50, float(row[4])); p99 = max(p99, float(row[6]))
print('%d %.0f %.0f %.2f %.3f %.3f %d %d' % (tot_n, tot_n / tmax, rsum, tmax, p50, p99, k, errs))
PY
)
summ() { python3 -c "$SUMPY" $O/out-$1-c1.txt $O/out-$1-c2.txt $O/out-$1-c3.txt; }
size_from() { # n per process for SECS s, from the pilot's slowest process on that host
	python3 - $O/out-$1-$2.txt $SECS <<'PY'
import sys, csv
r = [float(next(csv.reader([l.split(' ', 3)[3].strip()]))[1]) for l in open(sys.argv[1]) if l.startswith('RES ')]
if not r or min(r) <= 0: sys.exit(1)
print(int(min(r) * float(sys.argv[2])) // 1000 * 1000 + 1000)
PY
}
WINSH='snap() { redis-cli -h IP -p PORT info commandstats | tr -d "\r" | grep -E "^cmdstat_(set|get):" | sed "s/^[^:]*:calls=\([0-9]*\),.*/\1/" | awk "{s+=\$1} END{print s+0}"; }
t0=$(date +%s.%N); a=$(snap); sleep WINS; t1=$(date +%s.%N); b=$(snap); echo "$a $b $t0 $t1"'
window() {
	sleep 8
	local s=${WINSH//WINS/$WIN}; s=${s//IP/$IP}; s=${s//PORT/$PORT}
	on $A "bash -c '$s'" > $O/win-$1-raw.txt &
	local w=$!
	local c1a c2a c3a; c1a=$(on $B 'pgrep -xc redis-benchmark'); c2a=$(on $C 'pgrep -xc redis-benchmark'); c3a=$(pgrep -xc "$C3COMM")
	sleep $((WIN - 1))
	local c1 c2 c3; c1=$(on $B 'pgrep -xc redis-benchmark'); c2=$(on $C 'pgrep -xc redis-benchmark'); c3=$(pgrep -xc "$C3COMM")
	wait $w
	python3 - $O/win-$1-raw.txt $c1a $c2a $c1 $c2 $NP1 $NP2 $c3a $c3 $NP3 > $O/win-$1.txt <<'PY'
import sys
t = open(sys.argv[1]).read().split()
a, b, t0, t1 = int(t[0]), int(t[1]), float(t[2]), float(t[3])
ok = sys.argv[2] == sys.argv[4] == sys.argv[6] and sys.argv[3] == sys.argv[5] == sys.argv[7] and sys.argv[8] == sys.argv[9] == sys.argv[10]
print('%.0f %s clients alive start/end: c1 %s/%s of %s, c2 %s/%s of %s, c3 %s/%s of %s; %d in %.2f s' % ((b - a) / (t1 - t0), 'VALID' if ok else 'INVALID', sys.argv[2], sys.argv[4], sys.argv[6], sys.argv[3], sys.argv[5], sys.argv[7], sys.argv[8], sys.argv[9], sys.argv[10], b - a, t1 - t0))
PY
}
readback() {
	local out; out=$(for j in $(shuf -i 0-$((KEYS - 1)) -n 60); do echo "redis-cli -h $IP -p $PORT get 'key:$(printf %012d $j)' | wc -c"; done | on $A "bash -s")
	say "read-back: $(echo "$out" | awk -v v=$VAL '$1 > v / 2' | wc -l) of $(echo "$out" | wc -l) sampled keys present"
}

# ---- cells ---------------------------------------------------------------------------------------------
say "fill: 2 hosts x 2 x $KEYS SETs over keyspace $KEYS, $VAL B values"
F1=$((2 * KEYS / NP1)) F2=$((2 * KEYS / NP2)) F3=$((2 * KEYS / NP3))
drive fill SET $F1 $F2 $F3; say "  fill: $(summ fill)"
drive setpilot SET 1500000 1500000 1500000
n1=$(size_from setpilot c1) && n2=$(size_from setpilot c2) && n3=$(size_from setpilot c3) || { say "ABORT: SET pilot gave no rate"; exit 1; }
drive set SET $n1 $n2 $n3 & d=$!; sample set; window set; wait $d; wait
say "== SET: server answered $(cat $O/win-set.txt)"
say "   clients: $(summ set)   (requests, req/s by the longest client, sum of rates, longest s, p50 ms, p99 ms, procs, err lines)"
drive getpilot GET 1500000 1500000 1500000
n1=$(size_from getpilot c1) && n2=$(size_from getpilot c2) && n3=$(size_from getpilot c3) || { say "ABORT: GET pilot gave no rate"; exit 1; }
drive get GET $n1 $n2 $n3 & d=$!; sample get; window get; wait $d; wait
say "== GET: server answered $(cat $O/win-get.txt)"
say "   clients: $(summ get)"
readback
on $A "redis-cli -h $IP -p $PORT dbsize; redis-cli -h $IP -p $PORT info memory | grep -E '^used_memory_human'" > $O/dbsize-mem.txt
on $A "find $R/$TAG -type f ! -name 'server.*' ! -name check.txt" > $O/persist-files.txt
say "dbsize/memory: $(tr -d '\r' < $O/dbsize-mem.txt | tr '\n' ' '); files written by the server: $(wc -l < $O/persist-files.txt)"
on $A "cat $R/$TAG/server.log" > $O/server.log
say "server log: $(wc -l < $O/server.log) lines, $(grep -ciE 'error|crit|fatal' $O/server.log) error/crit/fatal, $(grep -ciE 'warn' $O/server.log) warn"
say "DONE $TAG"
