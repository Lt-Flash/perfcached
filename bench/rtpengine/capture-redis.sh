#!/bin/sh
# S29 proof obligation 1: capture the EXACT Redis command stream that
# rtpengine issues, so the perfcached RESP command list is settled from
# the wire and not from guesswork.
#
# ISOLATION (this host runs a LIVE rtpengine on table 0 / ng 2223 /
# ports 30000-40000 and a LIVE redis on 6379 - none of them are
# touched): our redis listens on 6399 with its own dir, our rtpengine
# runs table = -1 (userspace only, no kernel table), ng on 22223,
# media ports 41000-41020, all in this scratch dir, all killed on exit.
set -u
D=$(cd "$(dirname "$0")" && pwd)
RP=6399
NG=22223

cleanup() {
	[ -n "${RTP_PID:-}" ] && kill $RTP_PID 2>/dev/null
	[ -n "${MON_PID:-}" ] && kill $MON_PID 2>/dev/null
	[ -n "${RED_PID:-}" ] && kill $RED_PID 2>/dev/null
	sleep 0.3
	return 0
}
trap cleanup EXIT INT TERM

rm -f "$D"/*.log "$D"/*.rdb "$D"/monitor.txt
: > "$D/monitor.txt"

# ---- 1. an isolated redis, nothing shared with the live one ----------
redis-server --port $RP --bind 127.0.0.1 --dir "$D" \
	--save '' --appendonly no --daemonize no > "$D/redis.log" 2>&1 &
RED_PID=$!
i=0
while [ $i -lt 50 ]; do
	redis-cli -p $RP PING 2>/dev/null | grep -q PONG && break
	sleep 0.1; i=$((i+1))
done
redis-cli -p $RP PING 2>/dev/null | grep -q PONG || {
	echo "isolated redis did not start"; cat "$D/redis.log"; exit 1; }
echo "isolated redis up on $RP"

# ---- 2. MONITOR: every command rtpengine issues, verbatim -------------
redis-cli -p $RP MONITOR > "$D/monitor.txt" 2>&1 &
MON_PID=$!
sleep 0.5

# ---- 3. an isolated rtpengine pointed at it ---------------------------
cat > "$D/rtpengine.conf" <<EOF
[rtpengine]
table = -1
interface = 127.0.0.1
listen-ng = 127.0.0.1:$NG
port-min = 41000
port-max = 41020
foreground = true
log-stderr = true
log-level = 6
redis = 127.0.0.1:$RP/1
redis-write = 127.0.0.1:$RP/1
redis-expires = 86400
recording-dir = $D
EOF
/usr/bin/rtpengine --config-file "$D/rtpengine.conf" > "$D/rtpengine.log" 2>&1 &
RTP_PID=$!
sleep 2.5
kill -0 $RTP_PID 2>/dev/null || {
	echo "rtpengine did not start:"; tail -20 "$D/rtpengine.log"; exit 1; }
echo "isolated rtpengine up (pid $RTP_PID, ng $NG)"

# ---- 4. drive a real call through it ----------------------------------
python3 "$D/ngcall.py" $NG > "$D/ng.log" 2>&1
RC=$?
sleep 2                                # let the write thread flush

echo "--- ng exchange ---"
cat "$D/ng.log"
echo "--- rtpengine redis lines ---"
grep -i redis "$D/rtpengine.log" | head -20
cleanup
sleep 0.5
echo "--- captured command stream ---"
cat "$D/monitor.txt"
exit $RC
