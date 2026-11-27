#!/bin/bash
# valkeycluster.sh - a Valkey Cluster of NM instances (Debian's valkey-server package) on ONE NUMA node of
# SERVER (numactl), ports 7400.., cluster-enabled, no replicas, no io-threads, persistence off, slots split by
# valkey-cli --cluster create.  Clients and cells as sabench.sh (client 1 = CLIENT1, client 2 = CLIENT2,
# client 3 = this host), but routed: one hash tag per instance (crc16, checked with CLUSTER KEYSLOT), and one
# redis-benchmark process per instance per client host (120 / NM connections each, no --threads), so every
# key goes straight to its owner.
# Rate = INFO commandstats SET/GET calls summed over the instances, WIN s, every client alive.  Then 6 keys
# per instance read back from their owners, dbsize over all, files written besides logs/nodes.conf.
# Variables (required): SERVER, CLIENT1, CLIENT2 = ssh targets.  Optional: SERVER_IP (address the
#   instances bind, default SERVER), NM (10), NUMA (1), VAL (200), SECS (40), SSH_USER (root),
#   OUT (./results: results go to $OUT/<tag>/ here), WORK (/var/tmp/sabench on SERVER),
#   CLIENT2_SHELL, RB3, C3COMM as in sabench.sh.
# Requires: valkey-server and valkey-cli, numactl and python3 on SERVER; redis-benchmark 8.x on the
#   clients; python3 on every host; root ssh (BatchMode) to all three.
# Usage: SERVER=.. CLIENT1=.. CLIENT2=.. [NM=10|12|20] [VAL=32|200] bench/valkeycluster.sh <tag>
set -u
A=${SERVER:?set SERVER=<ssh target of the server host>}
B=${CLIENT1:?set CLIENT1=<ssh target of client 1>}
C=${CLIENT2:?set CLIENT2=<ssh target of client 2>}
OUT=${OUT:-./results}; R=${WORK:-/var/tmp/sabench}; SSH_USER=${SSH_USER:-root}
RB3=${RB3:-redis-benchmark}; C3COMM=${C3COMM:-redis-benchmark}; CLIENT2_SHELL=${CLIENT2_SHELL:-bash -s}
IP=${SERVER_IP:-$A} NUMA=${NUMA:-1} NM=${NM:-10}
KEYS=1000000 VAL=${VAL:-200} PIPE=32 SECS=${SECS:-40} WIN=15
on() { timeout ${T:-120} ssh -o BatchMode=yes -o ConnectTimeout=8 $SSH_USER@$1 "${@:2}"; }
ts() { date '+%T'; }
say() { echo "[$(ts)] $*" | tee -a $O/log.txt; }

TAG=${1:-}
O=$OUT/$TAG
[ -n "$TAG" ] && [ ! -e "$O" ] || { echo "ABORT: tag empty or $O exists"; exit 1; }
for h in $A $B $C; do
	on $h 'pgrep -x perfcached || pgrep -x valkey-server || pgrep -x dragonfly || pgrep -x redis-server || pgrep -x redis-benchmark || pgrep -x natbench' >/dev/null &&
		{ echo "ABORT: $h busy"; exit 1; }
done
pgrep -x "$C3COMM" >/dev/null && { echo "ABORT: this host (client 3) busy"; exit 1; }
mkdir -p $O

stop() {
	on $A "for f in \$(ls $R/$TAG/*/v.pid 2>/dev/null); do p=\$(cat \$f); [ \"\$(cat /proc/\$p/comm 2>/dev/null)\" = valkey-server ] && kill \$p; done
		for t in \$(seq 1 50); do pgrep -x valkey-server >/dev/null || break; sleep 0.2; done
		for f in \$(ls $R/$TAG/*/v.pid 2>/dev/null); do p=\$(cat \$f); [ \"\$(cat /proc/\$p/comm 2>/dev/null)\" = valkey-server ] && kill -9 \$p; done; true"
	on $A "cat $R/$TAG/*/v.log 2>/dev/null" > $O/server.log
	on $A "rm -rf $R/$TAG"
}
trap stop EXIT

# ---- start the cluster ----------------------------------------------------------------------------------
PORTS=$(seq -s ' ' 7400 $((7400 + NM - 1)))
on $A "bash -s" <<EOS || { say "ABORT: start"; exit 1; }
set -e
for p in $PORTS; do
	d=$R/$TAG/v\$p; mkdir -p \$d
	numactl --cpunodebind=$NUMA --membind=$NUMA valkey-server --bind $IP --port \$p --cluster-enabled yes \
		--cluster-config-file nodes.conf --cluster-node-timeout 15000 --save '' --appendonly no \
		--protected-mode no --dir \$d --daemonize yes --pidfile \$d/v.pid --logfile \$d/v.log
done
EOS
sleep 1
on $A "valkey-cli --cluster create $(for p in $PORTS; do printf '%s:%s ' $IP $p; done) --cluster-replicas 0 --cluster-yes" > $O/create.txt 2>&1 ||
	{ say "ABORT: cluster create"; tail -5 $O/create.txt; exit 1; }
for t in $(seq 1 120); do
	ok=$(on $A "for p in $PORTS; do valkey-cli -h $IP -p \$p cluster info | grep -c 'cluster_state:ok'; done" | grep -c '^1')
	[ "$ok" = $NM ] && break; sleep 0.5
done
[ "$ok" = $NM ] || { say "ABORT: cluster not ok ($ok of $NM)"; exit 1; }
P1=$(on $A "cat $R/$TAG/v7400/v.pid")
on $A "tr '\0' ' ' < /proc/$P1/cmdline; echo; grep -E 'Cpus_allowed_list' /proc/$P1/status; readlink /proc/$P1/exe; dpkg -S \$(readlink /proc/$P1/exe); pgrep -xc valkey-server" > $O/server-live.txt
say "Valkey Cluster: $NM masters on the server, NUMA $NUMA, $(on $A 'valkey-server --version' | cut -d' ' -f1-3) (package $(on $A "dpkg-query -W -f='\${Package} \${Version}' valkey-server")), no io-threads, $(tail -1 $O/server-live.txt) processes; $(sed -n 2p $O/server-live.txt | tr -s '\t ' ' ')"
on $A "for p in $PORTS; do echo \"s=\$(valkey-cli -h $IP -p \$p config get save | sed -n 2p) a=\$(valkey-cli -h $IP -p \$p config get appendonly | sed -n 2p) io=\$(valkey-cli -h $IP -p \$p config get io-threads | sed -n 2p)\"; done" > $O/persist.txt
say "persistence: $(sort $O/persist.txt | uniq -c | tr '\n' ' ')"
# ---- owners: one hash tag per master ---------------------------------------------------------------------
TAGPY=$(cat <<'PY'
import sys
def crc16(d):
    c = 0
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c
owners = []
for ln in open(sys.argv[1]):
    f = ln.split()
    if len(f) < 9 or 'master' not in f[2]:
        continue
    rng = []
    for r in f[8:]:
        if r.startswith('['):
            continue
        lo, _, hi = r.partition('-')
        rng.append((int(lo), int(hi or lo)))
    if rng:
        owners.append((f[1].split('@')[0], rng))
for addr, rng in sorted(owners):
    for i in range(200000):
        t = 't%d' % i
        s = crc16(t.encode()) & 16383
        if any(lo <= s <= hi for lo, hi in rng):
            print(addr.replace(':', ' '), t, s)
            break
    else:
        sys.exit('no tag for ' + addr)
PY
)
on $A "valkey-cli -h $IP -p 7400 cluster nodes" > $O/cluster-nodes.txt
python3 -c "$TAGPY" $O/cluster-nodes.txt > $O/owners.txt || { say "ABORT: owners"; exit 1; }
while read -r ip p t sl; do
	k=$(on $A "valkey-cli -h $ip -p $p cluster keyslot 'key:{$t}:0'")
	[ "$k" = "$sl" ] || { say "ABORT: tag $t -> slot $k at $ip:$p, computed $sl"; exit 1; }
done < $O/owners.txt
NK=$(wc -l < $O/owners.txt); [ $NK = $NM ] || { say "ABORT: $NK owners"; exit 1; }
say "owners: $NK, tags checked by CLUSTER KEYSLOT"

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
    if comm not in ('redis-benchmark', '@C3COMM@', 'valkey-server'):
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

# ---- clients: one redis-benchmark per master per host, c connections each, no --threads ---------------
cli_script() { # cli_script <SET|GET> <n per process> <conns>
	local ph=$1 nsrc=$2 c=$3 i=0 n r=$(( (KEYS + NK - 1) / NK ))
	echo "V=\$(printf '%${VAL}s' '' | tr ' ' x); B=\${RB:-redis-benchmark}; D=\$(mktemp -d)"
	while read -r ip p t sl; do i=$((i + 1)); n=$nsrc; [ -f "$nsrc" ] && n=$(sed -n "${i}p" "$nsrc")
		if [ $ph = SET ]; then cmd="SET key:{$t}:__rand_int__ \"\$V\""; else cmd="GET key:{$t}:__rand_int__"; fi
		echo "\$B -h $ip -p $p -n $n -c $c -P $PIPE -r $r --csv $cmd > \$D/o$i 2>\$D/e$i &"
	done < $O/owners.txt
	echo "wait"
	i=0; while read -r ip p t sl; do i=$((i + 1)); n=$nsrc; [ -f "$nsrc" ] && n=$(sed -n "${i}p" "$nsrc")
		echo "echo \"RES $i $n \$(grep -E '^\"(SET|GET)' \$D/o$i | tail -1)\"; [ -s \$D/e$i ] && sed 's/^/ERR $i /' \$D/e$i | head -3"
	done < $O/owners.txt
	echo "rm -rf \$D"
}
NP1=$NK NP2=$NK NP3=$NK C1=$((120 / NK)) C2=$((120 / NK)) C3=$((120 / NK))
drive() { # drive <cell> <SET|GET> <n c1> <n c2> <n c3>
	cli_script $2 $3 $C1 > $O/cli-$1-c1.sh
	cli_script $2 $4 $C2 > $O/cli-$1-c2.sh
	cli_script $2 $5 $C3 > $O/cli-$1-c3.sh
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
size_from() { # size_from <pilot> <host>: a file of n per process for SECS s, from each process's own pilot rate
	python3 - $O/out-$1-$2.txt $SECS > $O/n-$1-$2.txt <<'PY'
import sys, csv
rows = {}
for l in open(sys.argv[1]):
    if l.startswith('RES '):
        _, i, n, rest = l.split(' ', 3)
        rows[int(i)] = float(next(csv.reader([rest.strip()]))[1])
if not rows or min(rows.values()) <= 0: sys.exit(1)
for i in sorted(rows):
    print(int(rows[i] * float(sys.argv[2])) // 1000 * 1000 + 1000)
PY
	[ $? = 0 ] && [ "$(grep -c '^[1-9]' $O/n-$1-$2.txt)" = $NK ] || return 1
	echo $O/n-$1-$2.txt
}
WINSH='snap() { for p in PORTS; do valkey-cli -h IP -p $p info commandstats | tr -d "\r" | grep -E "^cmdstat_(set|get):"; done | sed "s/^[^:]*:calls=\([0-9]*\),.*/\1/" | awk "{s+=\$1} END{print s+0}"; }
t0=$(date +%s.%N); a=$(snap); sleep WINS; t1=$(date +%s.%N); b=$(snap); echo "$a $b $t0 $t1"'
window() {
	sleep 8
	local s=${WINSH//WINS/$WIN}; s=${s//IP/$IP}; s=${s//PORTS/$PORTS}
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
	local r=$(( (KEYS + NK - 1) / NK )) out
	out=$(while read -r ip p t sl; do for j in $(shuf -i 0-$((r - 1)) -n 6); do echo "valkey-cli -h $ip -p $p get 'key:{$t}:$(printf %012d $j)' | wc -c"; done; done < $O/owners.txt | on $A "bash -s")
	say "read-back: $(echo "$out" | awk -v v=$VAL '$1 > v / 2' | wc -l) of $(echo "$out" | wc -l) sampled keys present on their owners"
}

# ---- cells ---------------------------------------------------------------------------------------------
say "fill: 3 hosts x 2 x $KEYS SETs, keyspace $KEYS ($((KEYS / NK)) per master), $VAL B values"
F1=$((2 * KEYS / NK)) F2=$F1 F3=$F1
drive fill SET $F1 $F2 $F3; say "  fill: $(summ fill)"
drive setpilot SET 300000 300000 300000
n1=$(size_from setpilot c1) && n2=$(size_from setpilot c2) && n3=$(size_from setpilot c3) || { say "ABORT: SET pilot gave no rate"; exit 1; }
drive set SET $n1 $n2 $n3 & d=$!; sample set; window set; wait $d; wait
say "== SET: server answered $(cat $O/win-set.txt)"
say "   clients: $(summ set)   (requests, req/s by the longest client, sum of rates, longest s, p50 ms, p99 ms, procs, err lines)"
drive getpilot GET 300000 300000 300000
n1=$(size_from getpilot c1) && n2=$(size_from getpilot c2) && n3=$(size_from getpilot c3) || { say "ABORT: GET pilot gave no rate"; exit 1; }
drive get GET $n1 $n2 $n3 & d=$!; sample get; window get; wait $d; wait
say "== GET: server answered $(cat $O/win-get.txt)"
say "   clients: $(summ get)"
readback
on $A "for p in $PORTS; do valkey-cli -h $IP -p \$p dbsize; done" > $O/dbsize.txt
on $A "find $R/$TAG -type f ! -name 'v.*' ! -name nodes.conf" > $O/persist-files.txt
say "dbsize total $(awk '{s+=$1} END{print s}' $O/dbsize.txt) over $NK masters; files written besides logs/nodes.conf: $(wc -l < $O/persist-files.txt)"
on $A "cat $R/$TAG/*/v.log" > $O/server.log
say "server logs: $(wc -l < $O/server.log) lines, $(grep -c ' # ' $O/server.log) warning-level (#), $(grep -ciE 'error|fail' $O/server.log) error/fail"
say "DONE $TAG"
