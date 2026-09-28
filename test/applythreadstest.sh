#!/bin/sh
# applythreadstest.sh - S217: replica apply sharded over N threads keeps
# per-key order, loses nothing, and acks after the last record.
#
# One thread received, decrypted and applied every replicated record.
# With `[cluster] apply_threads = N` the receiver keeps the socket and
# hands each record to the thread its key hashes to, over that thread's
# own ring - a consistent-hash exchange, not a shared queue, because
# order per key is the correctness condition (S209).
#
# Two-node EAGER fleet, B the replica under test:
#  N = 4   - a writer through A: 40 hot keys, versions v1..v300 each,
#            interleaved, and 40 keys that alternate set/del ending set;
#            a READER on B polling the hot keys throughout must never see
#            a version go backwards (a value carries its own number); at
#            the end every hot key reads v300 on B and every set/del key
#            reads its final value; B's stats show 4 threads and every
#            record dispatched; the eager repair SWEEP (an acked group)
#            still completes - B answers its sync acks - measured as the
#            sender's sweep counter moving with no loss reported.
#  N = 1   - the same run: apply_threads = 1 is the receiver alone, and
#            the stats say so (1 thread, 0 dispatched).  S219: these two
#            arms are also what gates the collection being resolved ONCE
#            - N = 1 hands apply_rec() the handle the caller looked up,
#            N = 4 hands it NULL and it looks the handle up itself, and
#            both must reach the same end state from the same writes.
#  a value the ring cannot hold (60 KB is fine; the cap is 1 MB) - the
#            largest datagram record still crosses.
#  wedged   - ONE of B's four apply threads frozen (test/threadfreeze.c)
#            with apply_stall_ms = 2000 and records queued on it: B says
#            STALLED naming an apply thread (S216 sees 1/N of the keyspace
#            stuck as it sees all of it), the other three threads keep
#            applying, and B is READY again after the thread is released.
# Fail-first: a daemon without S217 refuses the config key, so the N = 4
# fleet does not start.  The order property is pinned where it can be
# told apart - clapplytest, where round-robin dispatch is 5952 versions
# backwards - because the table's own version check hides it here.
# Usage: test/applythreadstest.sh [./perfcached] [./threadfreeze]
set -u
BIN=${1:-./perfcached}
TF=${2:-./threadfreeze}
CLI=unused
SEC=at-client-secret
D=$(mktemp -d /var/tmp/pcat.XXXXXX)
PIDS=""
# a daemon under the freezer is the freezer's CHILD: kill -9 on the
# freezer orphans it, so the daemons are swept by their config path too
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

# conf <arm> <node> <threads> [extra]: 127.0.71.<arm><node>:1847<node>, group .24<arm>
conf() {
	cat > "$D/n$1$2.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = at-cluster-secret
[listen]
tcp = 127.0.71.$1$2:1847$2
plaintext = loopback
[cluster]
multicast = 239.255.77.24$1:1743$1
advertise = 127.0.71.$1$2
pull_timeout_ms = 400
mode = eager
collections = c
apply_threads = $3
${4:-}
[collection c]
buckets_log2 = 12
EOF
	chmod 600 "$D/n$1$2.conf"
}
start() { # <arm> <node>
	"$BIN" -f "$D/n$1$2.conf" > "$D/n$1$2.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1$2.log" 2>/dev/null && return 0
		kill -0 $! 2>/dev/null || break
		sleep 0.1; i=$((i + 1))
	done
	return 1
}
stop_arm() { for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3
	for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; sleep 0.3; }

DRV="$D/drv.py"
cat > "$DRV" <<'EOF'
# drv.py <cli> <secret> <arm>: writes through A, reads B, prints KEY=VALUE
import json, socket, subprocess, sys, threading, time
cli, sec, arm = sys.argv[1], sys.argv[2], sys.argv[3]
A = ("127.0.71.%s1" % arm, 18471); B = ("127.0.71.%s2" % arm, 18472)
HOT, ROUNDS, TOG = 40, 300, 40

class Conn:                                       # one plaintext loopback connection, many requests
    def __init__(s, n):
        s.f = socket.create_connection(n, timeout=30).makefile("rwb")
    def call(s, req):
        req = dict(req); req.setdefault("jsonrpc", "2.0"); req.setdefault("id", 1)
        s.f.write((json.dumps(req) + "\n").encode()); s.f.flush()
        return json.loads(s.f.readline())
    def close(s):
        try: s.f.close()
        except Exception: pass
def out(k, v): print("%s=%s" % (k, str(v).replace("\n", " "))); sys.stdout.flush()
def stats(n):
    c = Conn(n); r = c.call({"method": "stats"}); c.close(); return r.get("result", r)

a = Conn(A); b = Conn(B)
for _ in range(200):
    if stats(A).get("state") == "ready" and stats(B).get("state") == "ready": break
    time.sleep(0.1)
out("STATE0", "%s/%s" % (stats(A).get("state"), stats(B).get("state")))

# the reader: polls the hot keys on B while the writer runs
backwards = [0]; reads = [0]; stop = [0]
def reader():
    rb = Conn(B); last = {}
    while not stop[0]:
        for k in range(HOT):
            r = rb.call({"method": "get", "params": {"col": "c", "key": "hot%d" % k}}); r = r.get("result", r)
            v = r.get("value")
            if v is None: continue
            n = int(v.split(":")[1]); reads[0] += 1
            if n < last.get(k, 0): backwards[0] += 1
            last[k] = n
    rb.close()
t = threading.Thread(target=reader); t.start()

t0 = time.time()
for v in range(1, ROUNDS + 1):
    for k in range(HOT):
        a.call({"method": "set", "params": {"col": "c", "key": "hot%d" % k, "value": "hot%d:%d" % (k, v)}})
    if v % 10 == 0:
        for k in range(TOG):                       # set, del, set, del ... ends on set
            a.call({"method": "set", "params": {"col": "c", "key": "tog%d" % k, "value": "t%d:%d" % (k, v)}})
            if v % 20 == 0:
                a.call({"method": "del", "params": {"col": "c", "key": "tog%d" % k}})
                a.call({"method": "set", "params": {"col": "c", "key": "tog%d" % k, "value": "t%d:%d" % (k, v)}})
out("WRITE_S", "%.1f" % (time.time() - t0))
time.sleep(1.5)                                    # the last push group lands
stop[0] = 1; t.join()
out("READS", reads[0]); out("BACKWARDS", backwards[0])
# the end state on B
hot_ok = sum(1 for k in range(HOT) if (b.call({"method": "get", "params": {"col": "c", "key": "hot%d" % k}}).get("result") or {}).get("value") == "hot%d:%d" % (k, ROUNDS))
tog_ok = sum(1 for k in range(TOG) if (b.call({"method": "get", "params": {"col": "c", "key": "tog%d" % k}}).get("result") or {}).get("value") == "t%d:%d" % (k, ROUNDS))
out("HOT_FINAL", "%d/%d" % (hot_ok, HOT)); out("TOG_FINAL", "%d/%d" % (tog_ok, TOG))
# a value near the datagram ceiling
big = "B" * 60000
a.call({"method": "set", "params": {"col": "c", "key": "big", "value": big}})
time.sleep(0.5)
out("BIG", "ok" if (b.call({"method": "get", "params": {"col": "c", "key": "big"}}).get("result") or {}).get("value") == big else "no")
cb = stats(B).get("cluster") or {}
out("THREADS", cb.get("apply_threads")); out("DISPATCHED", cb.get("apply_dispatched")); out("BLOCKS", cb.get("apply_blocks"))
out("APPLIED", cb.get("rx_applied")); out("OLDER", cb.get("recv_older", cb.get("rx_older")))
# the ACKED path: a replica that restarts empty is backfilled in groups
# that carry a req and want an ack - which now goes out after the LAST
# record of the group is applied, whichever threads applied them.  The
# sender counts what was acked (migrated_out, S200) and what was not
# (migrate_lost, after three re-sends).
ca0 = stats(A).get("cluster") or {}
o0 = ca0.get("migrated_out", 0); l0 = ca0.get("migrate_lost", 0)
b.close()
import os
print("RESTART-B"); sys.stdout.flush()
for _ in range(300):
    if os.path.exists(sys.argv[4]): break
    time.sleep(0.1)
for _ in range(400):
    try:
        if stats(B).get("state") == "ready": break
    except Exception: pass
    time.sleep(0.1)
t1 = time.time(); have = 0
while time.time() - t1 < 30:
    b = Conn(B)
    have = sum(1 for k in range(HOT) if (b.call({"method": "get", "params": {"col": "c", "key": "hot%d" % k}}).get("result") or {}).get("value") == "hot%d:%d" % (k, ROUNDS))
    b.close()
    if have == HOT: break
    time.sleep(0.5)
out("BACKFILL_S", "%.1f" % (time.time() - t1)); out("BACKFILLED", "%d/%d" % (have, HOT))
time.sleep(2)
ca1 = stats(A).get("cluster") or {}
out("ACKED_OUT", "%s -> %s" % (o0, ca1.get("migrated_out", 0)))
out("LOST", "%s -> %s" % (l0, ca1.get("migrate_lost", 0)))
cb = stats(B).get("cluster") or {}
out("THREADS2", cb.get("apply_threads")); out("DISPATCHED2", cb.get("apply_dispatched"))
a.close()
EOF
val() { sed -n "s/^$2=//p" "$D/$1.out" | head -1; }
num() { case "${1:-}" in ''|*[!0-9]*) echo -1;; *) echo "$1";; esac; }

arm() { # arm <n> <threads>
	conf $1 1 $2; conf $1 2 $2
	start $1 1 && start $1 2 || return 1
	rm -f "$D/$1.restarted"
	python3 "$DRV" "$CLI" "$SEC" $1 "$D/$1.restarted" > "$D/$1.out" 2>&1 &
	DP=$!
	i=0
	while [ $i -lt 3000 ]; do
		grep -q "^RESTART-B" "$D/$1.out" 2>/dev/null && break
		kill -0 $DP 2>/dev/null || break
		sleep 0.1; i=$((i + 1))
	done
	if grep -q "^RESTART-B" "$D/$1.out" 2>/dev/null; then
		# the replica restarts EMPTY (no WAL): the backfill is the acked path
		for p in $PIDS; do grep -qa -- "n${1}2.conf" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done
		sleep 0.5
		mv "$D/n${1}2.log" "$D/n${1}2.log.1"
		start $1 2
		: > "$D/$1.restarted"
	fi
	wait $DP
}

echo "--- apply_threads = 4 on the replica"
if arm 1 4; then
	[ "$(val 1 STATE0)" = "ready/ready" ] && ok "a two-node eager fleet with apply_threads = 4" || bad "the fleet: $(val 1 STATE0) $(tail -1 "$D/1.out")"
	R=$(num "$(val 1 READS)")
	[ "$R" -ge 200 ] && [ "$(val 1 BACKWARDS)" = 0 ] \
		&& ok "a reader on the replica made $R reads of 40 hot keys during 12000 interleaved writes and never saw a version go backwards" \
		|| bad "reads $(val 1 READS), backwards $(val 1 BACKWARDS)"
	[ "$(val 1 HOT_FINAL)" = "40/40" ] && [ "$(val 1 TOG_FINAL)" = "40/40" ] \
		&& ok "every hot key ends at v300 and every set/del/set key at its final value on the replica" \
		|| bad "final state on the replica: hot $(val 1 HOT_FINAL), set/del $(val 1 TOG_FINAL)"
	[ "$(val 1 THREADS)" = 4 ] && [ "$(num "$(val 1 DISPATCHED)")" -ge 12000 ] \
		&& ok "the replica's stats: 4 apply threads, $(val 1 DISPATCHED) records handed over ($(val 1 BLOCKS) waits on a full ring), $(val 1 APPLIED) applied" \
		|| bad "stats: threads $(val 1 THREADS), dispatched $(val 1 DISPATCHED)"
	[ "$(val 1 BIG)" = ok ] && ok "a 60 KB value - the largest a datagram record can be - crosses" || bad "the 60 KB value did not arrive"
	O=$(val 1 ACKED_OUT); L=$(val 1 LOST)
	# Two roads bring a restarted replica back, and which one wins is a
	# race the environment decides: the SENDER's backfill (REPL_MANY
	# groups with a req, through the rings, acked after the last record)
	# or the node's own bootstrap pull over the bulk plane (S83), which
	# under a sanitizer's slowdown gets there first.  Either must leave
	# every key in place and nothing lost; the acked road is asserted
	# whenever it was the one taken.
	ROAD=$(grep -q "bootstrapped from node" "$D/n12.log" && echo bulk || echo groups)
	if [ "$(val 1 BACKFILLED)" = "40/40" ] && [ "${L##* }" = "${L%% *}" ] && [ "$(val 1 THREADS2)" = 4 ]; then
		if [ "$ROAD" = groups ]; then
			[ "$(num "$(val 1 DISPATCHED2)")" -ge 80 ] && [ "${O##* }" -gt "${O%% *}" ] \
				&& ok "the ACKED road: the replica restarted empty and the sender backfilled it in groups - $(val 1 DISPATCHED2) records through the rings, the sender's acked count $O, lost $L: the ack goes out after the group's last record" \
				|| bad "the sender's backfill: dispatched $(val 1 DISPATCHED2), acked $O, lost $L"
		else
			ok "the replica restarted empty and pulled its bootstrap over the bulk plane first (a race the sender's backfill lost here): 40/40 back, lost $L - the acked road is exercised by clapplytest and by this arm when it wins"
		fi
	else
		bad "backfill after restart: $(val 1 BACKFILLED) in $(val 1 BACKFILL_S) s (road: $ROAD), acked $O, lost $L, threads $(val 1 THREADS2), dispatched $(val 1 DISPATCHED2)"
	fi
	grep -q "sharded over 4 threads" "$D/n12.log" && ok "B's log: replica apply sharded over 4 threads by key hash" || bad "B's log lacks the apply-threads notice"
else
	bad "the apply_threads = 4 fleet did not start (a daemon without S217 refuses the key): $(grep -m1 ERROR "$D/n12.log" | cut -c1-120)"
fi
stop_arm

echo "--- apply_threads = 1 (the default): the receiver alone"
if arm 2 1; then
	[ "$(val 2 BACKWARDS)" = 0 ] && [ "$(val 2 HOT_FINAL)" = "40/40" ] && [ "$(val 2 TOG_FINAL)" = "40/40" ] \
		&& ok "the same run with apply_threads = 1: no version backwards, every key at its final value" \
		|| bad "apply_threads = 1: backwards $(val 2 BACKWARDS), hot $(val 2 HOT_FINAL), tog $(val 2 TOG_FINAL)"
	[ "$(val 2 THREADS)" = 1 ] && [ "$(val 2 DISPATCHED)" = 0 ] && ! grep -q "sharded over" "$D/n22.log" \
		&& ok "and the stats say so: 1 thread, 0 dispatched, no rings" || bad "apply_threads = 1 stats: threads $(val 2 THREADS), dispatched $(val 2 DISPATCHED)"
else
	bad "the default fleet did not start"
fi
stop_arm

# ---- one apply thread wedged ----------------------------------------------
echo "--- one of 4 apply threads frozen, apply_stall_ms = 2000"
if [ -x "$TF" ]; then
	conf 3 1 4 "apply_stall_ms = 2000"; conf 3 2 4 "apply_stall_ms = 2000"
	: > "$D/ctl3"
	start 3 1
	"$TF" "$D/ctl3" -- "$BIN" -f "$D/n32.conf" > "$D/n32.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0; while [ $i -lt 200 ]; do grep -q "perfcached ready" "$D/n32.log" 2>/dev/null && break; sleep 0.1; i=$((i + 1)); done
	python3 - "$D/ctl3" > "$D/3.out" 2>&1 <<'PY'
import json, os, socket, sys, time
ctl = sys.argv[1]
def call(n, req):
    f = socket.create_connection(("127.0.71.3%d" % n, 18470 + n), timeout=10).makefile("rwb")
    req.setdefault("jsonrpc", "2.0"); req.setdefault("id", 1)
    f.write((json.dumps(req) + "\n").encode()); f.flush(); r = json.loads(f.readline()); f.close(); return r.get("result", r)
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
for _ in range(200):
    if call(1, {"method": "stats"}).get("state") == "ready" and call(2, {"method": "stats"}).get("state") == "ready": break
    time.sleep(0.1)
open(ctl, "w").write("1 pc-apply 9000\n")
for _ in range(50):
    if os.path.exists(ctl + ".ack") and "frozen" in open(ctl + ".ack").read(): break
    time.sleep(0.1)
out("ACK", open(ctl + ".ack").read().strip() if os.path.exists(ctl + ".ack") else "none")
t0 = time.time()
for i in range(400):
    call(1, {"method": "set", "params": {"col": "c", "key": "w%d" % i, "value": "v"}})
st = None
while time.time() - t0 < 8:
    s = call(2, {"method": "stats"}); st = s.get("state"); c = s.get("cluster") or {}
    if st == "stalled": break
    time.sleep(0.2)
out("STALLED_AFTER_MS", int((time.time() - t0) * 1000) if st == "stalled" else -1)
out("STATE1", st); out("REASON1", s.get("state_reason", "")); out("BACKLOG1", c.get("apply_backlog"))
# what B applied vs what was handed to its threads: the frozen thread's
# share is the gap (a GET would hide it - a replica pulls a key it misses)
out("APPLIED1", c.get("rx_applied")); out("DISPATCHED1", c.get("apply_dispatched"))
for _ in range(120):
    if os.path.exists(ctl + ".ack") and "released" in open(ctl + ".ack").read(): break
    time.sleep(0.1)
t1 = time.time()
while time.time() - t1 < 8:
    s = call(2, {"method": "stats"}); c = s.get("cluster") or {}
    if s.get("state") == "ready" and c.get("apply_backlog") == 0: break
    time.sleep(0.2)
out("READY_AFTER_MS", int((time.time() - t1) * 1000) if s.get("state") == "ready" else -1)
out("BACKLOG2", c.get("apply_backlog")); out("STALLS", c.get("apply_stalls")); out("APPLIED2", c.get("rx_applied"))
PY
	case "$(val 3 ACK)" in *" frozen "*)
		T=$(num "$(val 3 STALLED_AFTER_MS)"); V1=$(num "$(val 3 VISIBLE1)")
		[ "$(val 3 STATE1)" = stalled ] && [ "$T" -ge 1500 ] && [ "$T" -le 5000 ] \
			&& ok "one apply thread frozen with records queued on it: B says STALLED $T ms in (limit 2000), backlog $(val 3 BACKLOG1) bytes" \
			|| { bad "one apply thread frozen: B is '$(val 3 STATE1)' after $(val 3 STALLED_AFTER_MS) ms (backlog $(val 3 BACKLOG1))"
			     # rc34's GitLab check-asan read STALLED 133 ms after the
			     # writes began - before the frozen thread could have held
			     # a record for 2 s - and two stalls; not reproduced here
			     # (1-2 cores, a CPU hog beside it).  B's own stall lines
			     # say which thread stalled, and when, next time:
			     grep -E "made no progress|STALLED|stalled|progress again" "$D/n32.log" | cut -c1-200 | sed 's/^/    B: /' | head -8; }
		case "$(val 3 REASON1)" in *"cluster thread"*) ok "with the S216 reason";; *) bad "reason: '$(val 3 REASON1)'";; esac
		grep -q "made no progress.*an apply thread with records queued" "$D/n32.log" && ok "and the log names the apply thread" || bad "the log: $(grep -m1 "made no progress" "$D/n32.log" | cut -c1-140)"
		A1=$(num "$(val 3 APPLIED1)"); D1=$(num "$(val 3 DISPATCHED1)")
		[ "$D1" -ge 400 ] && [ "$A1" -ge $((D1 * 55 / 100)) ] && [ "$A1" -le $((D1 * 90 / 100)) ] \
			&& ok "the other three threads kept applying: $A1 of $D1 records applied while one thread's share waited in its ring" \
			|| bad "applied while frozen: $(val 3 APPLIED1) of $(val 3 DISPATCHED1) dispatched (want ~3/4)"
		R=$(num "$(val 3 READY_AFTER_MS)")
		[ "$R" -ge 0 ] && [ "$(val 3 BACKLOG2)" = 0 ] && [ "$(num "$(val 3 APPLIED2)")" -ge "$D1" ] && [ "$(val 3 STALLS)" = 1 ] \
			&& ok "released: the backlog drains ($(val 3 APPLIED2) applied), READY again after $R ms, 1 stall counted" \
			|| bad "after release: ready after $(val 3 READY_AFTER_MS) ms, backlog $(val 3 BACKLOG2), applied $(val 3 APPLIED2) of $D1, stalls $(val 3 STALLS)";;
	*) bad "the apply thread was not frozen: $(val 3 ACK) - $(grep -v "^[A-Z0-9_]*=" "$D/3.out" | tail -2 | tr '\n' ' ' | cut -c1-200) / n32: $(tail -2 "$D/n32.log" | cut -c1-200 | tr '\n' ' ')";; esac
	stop_arm
else
	echo "  (no $TF: the wedged-apply-thread arm skipped)"
fi

echo "applythreadstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
