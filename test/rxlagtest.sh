#!/bin/sh
# rxlagtest.sh - a node that cannot keep up with inbound replicas says so: its rx_ figures, sampled
# while node 1 is driven, in stats, metrics and the page.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrx.XXXXXX)
P1="" P2=""
trap 'for p in $P1 $P2; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

node() { # node <n>: client 179<n>1, http 180<n>1, advertise 127.0.1.3<n>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = rx-client-secret
cluster = rx-cluster-secret
[listen]
tcp = 127.0.0.1:179${1}1
http = 127.0.0.1:180${1}1
plaintext = loopback
[cluster]
multicast = 239.255.77.51:17151
advertise = 127.0.1.3${1}
pull_timeout_ms = 300
[collection c]
buckets_log2 = 16
pull = 1
mode = eager
CONF
}
for n in 1 2; do node $n; done
for n in 1 2; do
	"$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	eval "P$n=\$!"
done
i=0
while [ $i -lt 200 ]; do
	if grep -q "perfcached ready" "$D/n1.log" 2>/dev/null &&
	   grep -q "perfcached ready" "$D/n2.log" 2>/dev/null; then break; fi
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n2.log" || { echo "nodes did not start"; cat "$D/n1.log" "$D/n2.log"; exit 1; }
# rx <port> <field>  - one field of stats.cluster, MISSING when absent
rx() {
	python3 - "$1" "$2" <<'PY'
import json,pcnative,socket,sys
s=socket.create_connection(("127.0.0.1",int(sys.argv[1])),timeout=20);f=pcnative.wrap(s)
f.write((json.dumps({"jsonrpc":"2.0","id":1,"method":"stats"})+"\n").encode());f.flush()
c=json.loads(f.readline()).get("result",{}).get("cluster",{})
v=c.get(sys.argv[2])
print("MISSING" if v is None else v)
PY
}

# both nodes must see each other before the drive: an eager push goes to live peers only, and
# writes made while node 2 joins are left to the repair sweep
i=0; seen=0
while [ $i -lt 300 ]; do
	if [ "$(rx 17911 peers_up)" = 1 ] && [ "$(rx 17921 peers_up)" = 1 ]; then
		seen=$((seen+1))
		[ $seen -ge 3 ] && break
	else
		seen=0
	fi
	sleep 0.2; i=$((i+1))
done
[ $seen -ge 3 ] \
	&& ok "both nodes see each other before the drive" \
	|| { echo "the pair never formed (peers_up $(rx 17911 peers_up) / $(rx 17921 peers_up))"; exit 1; }
# and SETTLED: peers_up flips on the first beat, while the joiner still connects and bootstraps -
# until both are ready (up to 30 s), or a drive started then leaves most records to the repair sweep
rdy() {
	python3 - "$1" <<'PY'
import json,pcnative,socket,sys
s=socket.create_connection(("127.0.0.1",int(sys.argv[1])),timeout=20);f=pcnative.wrap(s)
f.write((json.dumps({"jsonrpc":"2.0","id":1,"method":"stats"})+"\n").encode());f.flush()
print(json.loads(f.readline()).get("result",{}).get("state"))
PY
}
i=0; while [ $i -lt 150 ] && [ "$(rdy 17911 2>/dev/null)$(rdy 17921 2>/dev/null)" != readyready ]; do sleep 0.2; i=$((i+1)); done

# 1. the figures exist at all, and the buffer is the one the daemon read
#    back from the kernel rather than the one it asked for
for fld in rx_applied rx_applied_ps rx_older_ps rx_drops rx_drops_ps rx_queue rx_rcvbuf; do
	[ "$(rx 17921 $fld)" = MISSING ] && bad "stats.cluster has no $fld"
done
RB=$(rx 17921 rx_rcvbuf)
[ "$RB" != MISSING ] && [ "$RB" -gt 0 ] \
	&& ok "the receive plane reports itself: buffer $RB bytes, effective" \
	|| bad "rx_rcvbuf is $RB"
A0=$(rx 17921 rx_applied)
[ "$A0" = 0 ] && ok "nothing applied yet on the quiet node" || bad "rx_applied starts at $A0"
[ "$A0" = MISSING ] && A0=-1        # so the climb check below can still run

# 2. drive node 1 and sample node 2 WHILE it runs; every reading goes to a file so the peak is
#    read from the whole window
: > "$D/samples"
( i=0; while [ $i -lt 400 ] && [ ! -f "$D/done" ]; do
	echo "$(rx 17921 rx_applied_ps) $(rx 17921 rx_queue) $(rx 17921 rx_drops)" >> "$D/samples"
	i=$((i+1))
  done ) &
SAMP=$!
python3 - <<'PY'
import json, pcnative, socket, threading
def drive(tag, n):
    s = socket.create_connection(("127.0.0.1", 17911), timeout=180); f = pcnative.wrap(s)
    infl = 0
    for i in range(n):
        f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
            {"col":"c","key":"%s%07d"%(tag,i),"value":"x"*120,"ttl":600}})+"\n").encode())
        infl += 1
        if infl == 2000:
            f.flush()
            for _ in range(2000): f.readline()
            infl = 0
    f.flush()
    for _ in range(infl): f.readline()
    s.close()
ts=[threading.Thread(target=drive,args=("w%d"%k, 60000)) for k in range(3)]
for t in ts: t.start()
for t in ts: t.join()
print("drove 180000")
PY
touch "$D/done"; wait $SAMP 2>/dev/null
PEAK=$(awk '{if($1>m)m=$1}END{print m+0}' "$D/samples")
QPEAK=$(awk '{if($2>m)m=$2}END{print m+0}' "$D/samples")
DPEAK=$(awk '{if($3>m)m=$3}END{print m+0}' "$D/samples")
NS=$(wc -l < "$D/samples")
echo "  ..   $NS samples during the drive: applied/s peak $PEAK, queue peak $QPEAK, drops $DPEAK"
[ "$PEAK" -gt 0 ] \
	&& ok "the applied rate is nonzero WHILE the drive runs (peak ${PEAK}/s over $NS samples)" \
	|| bad "applied/s stayed 0 through the whole drive - the rate is not being taken"

# 3. what the sender pushed is what the receiver applied.  Eager push is
#    fire-and-forget and the sweep repairs the rest, so this settles.
i=0
while [ $i -lt 60 ]; do
	PUSHED=$(rx 17911 repl_pushed); APPLIED=$(rx 17921 rx_applied)
	[ "$PUSHED" != MISSING ] && [ "$APPLIED" = "$PUSHED" ] && break
	sleep 0.5; i=$((i+1))
done
[ "$APPLIED" = "$PUSHED" ] \
	&& ok "every record the sender pushed was applied ($APPLIED of $PUSHED)" \
	|| bad "applied $APPLIED against $PUSHED pushed"
# and the push covered the drive, not a corner of it - see the peer wait
{ [ "$PUSHED" != MISSING ] && [ "$PUSHED" -ge 170000 ]; } \
	&& ok "the push covered the drive ($PUSHED of 180,000 written)" \
	|| bad "only $PUSHED of 180,000 were pushed - the peer was not up for most of it"
{ [ "$APPLIED" != MISSING ] && [ "$APPLIED" -gt "$A0" ]; } \
	&& ok "the applied total climbed ($A0 -> $APPLIED)" \
	|| bad "the applied total did not move ($A0 -> $APPLIED)"

# 4. the rate falls back to zero when the drive stops - a gauge stuck at
#    its peak would pass every check above
i=0
while [ $i -lt 60 ]; do
	[ "$(rx 17921 rx_applied_ps)" = 0 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$(rx 17921 rx_applied_ps)" = 0 ] \
	&& ok "the rate returns to 0 once the fleet is quiet" \
	|| bad "applied/s stuck at $(rx 17921 rx_applied_ps) with nothing being written"

# 5. the sampler runs on the maintenance thread, not the cluster thread: the rate moved during
#    a drive that kept the cluster thread fully occupied (item 2), and the totals agree (item 3)

# 6. the metrics carry the same figures
M=$(python3 - <<'PY'
import urllib.request
print(urllib.request.urlopen("http://127.0.0.1:18021/metrics", timeout=10).read().decode())
PY
)
for g in rx_applied_total rx_applied_per_second rx_older_per_second rx_drops_total rx_drops_per_second rx_queue_bytes rx_rcvbuf_bytes; do
	echo "$M" | grep -q "perfcached_cluster_$g " || bad "metrics have no perfcached_cluster_$g"
done
echo "$M" | grep -q "perfcached_cluster_rx_applied_total " && ok "the metrics carry the replica-intake gauges"
# the same total off both, read at ONE moment: the sweep can still move the applied count after
# item 3, so a metrics read bracketed by two equal stats reads compares like for like
i=0
while [ $i -lt 20 ]; do
	S1=$(rx 17921 rx_applied)
	V=$(python3 -c 'import urllib.request; print(urllib.request.urlopen("http://127.0.0.1:18021/metrics", timeout=10).read().decode())' |
		awk '/^perfcached_cluster_rx_applied_total /{print $2}')
	S2=$(rx 17921 rx_applied)
	[ "$S1" = "$S2" ] && break
	sleep 0.5; i=$((i+1))
done
[ "$S1" = "$S2" ] && [ "$V" = "$S1" ] && ok "the metric and the stats agree on the applied total ($V)" \
	|| bad "metrics say $V, stats say $S1 then $S2"

# 7. the page has the card; the daemon serves no page (GET / is 410), its bytes come from
#    pagedump, the dashboard's object
PG=$("${PAGEDUMP:-./pagedump}")
echo "$PG" | grep -q "replica intake" && ok "the status page has the replica-intake card" \
	|| bad "the page has no replica-intake card"
echo "$PG" | grep -q "rx_applied_ps" && ok "and it reads the rate, not just the total" \
	|| bad "the card does not read rx_applied_ps"

echo "rxlagtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
