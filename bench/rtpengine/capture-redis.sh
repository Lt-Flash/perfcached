#!/bin/sh
# S29 proof obligation 1: capture the EXACT Redis command stream that
# rtpengine issues, so the perfcached RESP command list is settled from
# the wire and not from guesswork.
#
# ISOLATION (this host runs a LIVE rtpengine on table 0 / ng 2223 /
# ports 30000-40000 - none of them are touched): our redis is a
# CONTAINER, redis 8, the same image every other bench here uses,
# published on 127.0.0.1:6399 and nowhere else; our rtpengine runs
# table = -1 (userspace only, no kernel table), ng on 22223, media ports
# 41000-41020, all in this scratch dir, all removed on exit.
#
# rtpengine stays a HOST binary, deliberately: it is the subject under
# test and the whole point is to capture what the host's own rtpengine
# sends.  That is why redis publishes a port rather than living on a
# bench network - the client cannot be moved onto the network with it.
set -u
D=$(cd "$(dirname "$0")" && pwd)
. "$D/../containerlib.sh"
cl_prog=capture-redis
RP=6399
NG=22223
RCON=cap-redis

cleanup() {
	[ -n "${RTP_PID:-}" ] && kill $RTP_PID 2>/dev/null
	[ -n "${MON_PID:-}" ] && kill $MON_PID 2>/dev/null
	cl_rm "$RCON"
	sleep 0.3
	return 0
}
trap cleanup EXIT INT TERM

cl_runtime_pick || exit 1

rm -f "$D"/*.log "$D"/*.rdb "$D"/monitor.txt
: > "$D/monitor.txt"

# ---- 1. an isolated redis, nothing shared with the live one ----------
CL_RUN_EXTRA="-p 127.0.0.1:$RP:6379"
cl_redis_up "$RCON" "" || {
	echo "isolated redis did not start"
	$RT logs "$RCON" 2>&1 | tail -20
	exit 1; }
echo "isolated redis up on $RP ($RT, $CL_REDIS_IMG)"

# ---- 2. MONITOR: every command rtpengine issues, verbatim -------------
cl_redis_exec "$RCON" MONITOR > "$D/monitor.txt" 2>&1 &
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
