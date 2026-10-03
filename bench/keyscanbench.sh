#!/bin/sh
# keyscanbench.sh - KEYS and SCAN against Redis, on keyspaces shaped like
# two production Redis instances, with the cost that matters measured:
# what a key walk does to everyone ELSE's GETs.
#
# Redis runs one command at a time, so a KEYS that walks 74,000 keys holds
# every other client for its whole duration (one production instance: 186,745
# KEYS calls at 9.45 ms each).  perfcached's KEYS is cooperative - a chunk
# per event-loop turn - and its workers keep serving.  This puts numbers
# on both sides, three ways:
#
#   keys   KEYS <pattern> back to back: per-call latency (narrow - the
#          production lookup, ~1 match - and broad, ~4,400 matches)
#   scan   the same lookup as a SCAN MATCH walk, COUNT 1000 and 100:
#          walk time and round trips; every walk is checked against KEYS
#   stall  50 connections of GETs on the session keys (pcbench, depth 1,
#          per-op p50/p99/p999) while one client sends KEYS at 0, 10 and
#          50 per second - the GETs' throughput and tail are the result
#
# Datasets and patterns: bench/keyscan.py (shapes only - no production
# key or value is in this repository).
#
# Rig: both servers on SERVER (containers, --network host, the same
# cpuset), the client here.  Arms alternate within every rep; median of
# REPS.  Refuses a busy server host, as xhostbench.sh does.
#
#   SERVER=<ip> bench/keyscanbench.sh [reps]
#   DATASETS="mixed tokens" RATES="0 10 50" CPUSET=0-3 WORKERS=4
set -u
SERVER=${SERVER:?set SERVER=<ip of the host that runs both servers>}
REPS=${1:-3}
DATASETS=${DATASETS:-"mixed tokens"}
RATES=${RATES:-"0 10 50"}
CPUSET=${CPUSET:-0-3}
WORKERS=${WORKERS:-4}
SECS=${SECS:-12}                  # stall cell: pcbench runs SECS, warms 2
CONNS=${CONNS:-50}
REV=${REV:-v0.4.0}                # the tree whose daemon is measured
OUT=${OUT:-bench/results/keyscanbench.tsv}
IMG=perfcached:ksb
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
RPORT=16700; PPORT=16701; NPORT=16702
RCON=ksb-redis; PCON=ksb-perf
TOP=$(cd "$(dirname "$0")/.." && pwd)
PY="python3 $TOP/bench/keyscan.py"
PB=$TOP/pcbench
JWT_PFX=$(sed -n 's/^JWT_PFX = "\([^"]*\)".*/\1/p' "$TOP/bench/keyscan.py")
ME=$(ip route get "$SERVER" | grep -oE 'src [0-9.]+' | awk '{print $2}')
ss_() { ssh -o ConnectTimeout=10 "root@$SERVER" "$@"; }

[ -x "$PB" ] || { echo "keyscanbench: build pcbench first (make pcbench)"; exit 1; }
[ -n "$JWT_PFX" ] || { echo "keyscanbench: no JWT_PFX in keyscan.py"; exit 1; }
BUSY=$(ss_ 'ps -eo comm --no-headers | grep -cE "^(perfcached|pcbench|make|cc1|redis-benchmark|redis-server)$"')
[ "${BUSY:-1}" -gt 0 ] && { echo "keyscanbench: $SERVER is busy ($BUSY procs) - refusing"; exit 1; }

# the daemon from REV's tree, built on the server host like xhostbench's
SRC=/var/tmp/ksb-src
git -C "$TOP" archive --prefix=ksb-src/ "$REV" | ss_ "rm -rf $SRC && tar -x -C /var/tmp" \
	|| { echo "keyscanbench: cannot export $REV"; exit 1; }
BREV=$(git -C "$TOP" rev-parse --short "$REV^{commit}")
ss_ "cd $SRC && podman build -q --platform linux/amd64 -f bench/Containerfile.debian \
	--build-arg REV=$BREV -t $IMG . >/dev/null 2>&1" || { echo "keyscanbench: image build failed"; exit 1; }
BINREV=$(ss_ "podman run --rm --entrypoint perfcached $IMG -V 2>/dev/null" | head -1)
RSV=$(ss_ "podman run --rm --entrypoint redis-server $REDIS_IMG --version" | grep -oE 'v=[0-9.]+' | cut -c3-)
case "$BINREV" in *"($BREV)"*) ;; *) echo "keyscanbench: the image says '$BINREV', not $BREV - refusing"; exit 1;; esac
echo "server $SERVER: $BINREV, redis $RSV, cpuset $CPUSET, $WORKERS workers; client $ME"

D=$(ss_ 'mktemp -d /var/tmp/ksb.XXXXXX')
stop() { ss_ "podman rm -f $RCON $PCON >/dev/null 2>&1"; }
trap 'stop; ss_ "rm -rf $D $SRC"; rm -rf "$TMP"' EXIT
TMP=$(mktemp -d)
ss_ "cat > $D/pc.conf <<EOF
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = ksb-client-secret
[listen]
tcp = $SERVER:$NPORT
resp_allow = $ME/32
resp = $SERVER:$PPORT
[collection 0]
buckets_log2 = 17
EOF
chmod 644 $D/pc.conf"

start() {
	stop
	ss_ "podman run -d --name $RCON --network host --cpuset-cpus=$CPUSET $REDIS_IMG \
		--port $RPORT --bind $SERVER --protected-mode no --save '' --appendonly no >/dev/null" || return 1
	ss_ "podman run -d --name $PCON --network host --cpuset-cpus=$CPUSET --ulimit memlock=-1 \
		-v $D/pc.conf:/etc/perfcached.conf:ro $IMG >/dev/null" || return 1
	for port in $RPORT $PPORT; do
		i=0; until $PY ping "$SERVER" $port x >/dev/null 2>&1 || [ $i -ge 100 ]; do sleep 0.2; i=$((i+1)); done
	done
}
port_of() { [ "$1" = redis ] && echo $RPORT || echo $PPORT; }

for ds in $DATASETS; do
	echo "=== $ds"
	start || { echo "keyscanbench: servers did not start"; exit 1; }
	for arm in redis perfcached; do
		$PY load "$SERVER" "$(port_of $arm)" "$ds" | sed "s/^/  $arm: /" || { echo "keyscanbench: $arm load failed"; exit 1; }
	done
	NJWT=$($PY njwt "$SERVER" $RPORT "$ds")
	# SETTLE: perfcached sizes its table to the load after the fact -
	# 74k keys into the configured 131,072 buckets shrink to 32,768
	# 40-60 s later (measured 2026-10-01).  A walk visits buckets, so the
	# first rep of the first run ran on the big table: KEYS 11 ms against
	# ~5 ms, SCAN 73 calls against 33.  Measure the table the daemon keeps.
	sleep "${SETTLE:-75}"
	for rep in $(seq 1 "$REPS"); do
		for arm in redis perfcached; do
			p=$(port_of $arm)
			{
			$PY keys "$SERVER" "$p" "$ds" narrow 200 0
			$PY keys "$SERVER" "$p" "$ds" broad 20 0
			$PY scan "$SERVER" "$p" "$ds" narrow 1000 10
			$PY scan "$SERVER" "$p" "$ds" narrow 100 3
			for rate in $RATES; do
				$PB -h "$SERVER" -p "$p" -P resp -K "$JWT_PFX" -n "$NJWT" -N -M 100 \
					-c "$CONNS" -d 1 -T "$SECS" -w 2 > "$TMP/pb" 2>&1 &
				pbp=$!
				if [ "$rate" -gt 0 ]; then
					$PY keys "$SERVER" "$p" "$ds" narrow $((rate * SECS)) "$rate" > "$TMP/kd"
				else
					: > "$TMP/kd"
				fi
				wait $pbp
				echo "stall rate=$rate $(tail -1 "$TMP/pb") | $(cat "$TMP/kd")"
			done
			} | sed "s/^/$ds $arm rep$rep /" | tee -a "$TMP/raw"
		done
	done
	stop
done

# ---- report: medians of REPS, one row per dataset/test/arm -----------
mkdir -p "$(dirname "$OUT")"
python3 - "$TMP/raw" "$BINREV" "$RSV" "$REPS" "$CPUSET" "$WORKERS" "$CONNS" "$SECS" > "$OUT" <<'PY'
import re, sys, statistics, time
raw, binrev, rsv, reps, cpus, workers, conns, secs = sys.argv[1:]
print("# perfcached=%s redis=%s reps=%s cpuset=%s workers=%s stall_conns=%s stall_secs=%s date=%s" %
      (binrev, rsv, reps, cpus, workers, conns, secs, time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime())))
print("dataset\ttest\tarm\tmetric\tmedian\treps")
rows = {}
for line in open(raw):
    f = line.split()
    ds, arm = f[0], f[1]
    rest = line.split(None, 3)[3]
    if rest.startswith("keys "):
        m = dict(re.findall(r"(\w+)=([\d.]+)", rest)); test = "keys_" + rest.split()[1]
        vals = {k: m[k] for k in ("p50_ms", "p99_ms", "matched_per_call")}
    elif rest.startswith("scan "):
        m = dict(re.findall(r"(\w+)=([\d.]+)", rest)); test = "scan_%s_count%s" % (rest.split()[1], m["count"])
        vals = {k: m[k] for k in ("p50_ms", "p99_ms", "calls_per_walk", "matched_per_walk")}
    elif rest.startswith("stall "):
        rate = re.search(r"rate=(\d+)", rest).group(1); test = "stall_keys%sps" % rate
        pb = re.search(r"([\d.]+) ops/s\s+p50=(\d+)us p99=(\d+)us p999=(\d+)us", rest)
        if not pb:
            print("# NO RESULT %s %s %s: %s" % (ds, arm, test, rest.strip())); continue
        vals = {"get_ops_s": pb.group(1), "get_p50_us": pb.group(2), "get_p99_us": pb.group(3), "get_p999_us": pb.group(4)}
        kd = re.search(r"calls=(\d+) rate=[\d.]+ p50_ms=([\d.]+) p99_ms=([\d.]+).* elapsed_s=([\d.]+)", rest)
        if kd:
            vals["keys_p50_ms"], vals["keys_p99_ms"] = kd.group(2), kd.group(3)
            # a slow KEYS cannot hold an open-loop schedule from one
            # client: report the rate it actually reached
            vals["keys_achieved_per_s"] = "%.2f" % (int(kd.group(1)) / float(kd.group(4)))
    else:
        continue
    for k, v in vals.items():
        rows.setdefault((ds, test, arm, k), []).append(float(v))
for (ds, test, arm, k), v in rows.items():
    print("%s\t%s\t%s\t%s\t%g\t%s" % (ds, test, arm, k, statistics.median(v), "/".join("%g" % x for x in v)))
PY
echo "results: $OUT"
