#!/bin/sh
# winstatstest.sh - S351: /stats carries, beside every counter the status page
# shows, its window over the last five minutes (win_<field>), kept by the
# daemon.
#
# The page reads as a five-minute dashboard and was one only for the
# commands table: the rest were lifetime totals under a "last 5 min"
# heading (PROD 2026-10-08: "All these numbers don't look like 5 min ones").
# wintest.c pins the window arithmetic on an injected clock; this pins the
# wiring, on two eager members:
#  - every win_ field the page reads is in /stats (a missing one would show
#    a dash forever);
#  - on a daemon younger than five minutes a window is its whole count -
#    50 stores, 20 hits, 1 miss;
#  - reset stats: the window counts from the reset, not across it;
#  - the FLEET windows are null while a member has no minute of baseline
#    (never a few seconds passed off as five minutes), and a figure once
#    it has one;
#  - since.win_s says how far back the windows reach.
# ~75 s: the fleet case needs a minute of baseline.
# Usage: test/winstatstest.sh [./perfcached] [./perfcli]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcwin.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

node() { # node <id> <native port> <http port>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = win-client-secret
cluster = win-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[wal]
dir = $D/wal$1
probe = no
fsync = everysec
save = 900 1
[cluster]
multicast = 239.255.77.51:17151
advertise = 127.0.51.$1
[collection 0]
buckets_log2 = 10
mode = eager
CONF
	chmod 600 "$D/n$1.conf"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
node 1 17951 17952
node 2 17953 17954

cli() { "$CLI" -q -h 127.0.0.1 -p 17951 "$@" > /dev/null 2>&1; }

# node 1 ready (it refuses writes while recovering) and both members
# reporting the collection before anything is counted
i=0
while [ $i -lt 100 ]; do
	n=$(python3 -c "import json,urllib.request; s=json.load(urllib.request.urlopen('http://127.0.0.1:17952/stats', timeout=3)); print(int(s['state'] == 'ready' and (s['collections'][0].get('fleet') or {}).get('reporting', 0) >= 2))" 2>/dev/null)
	[ "${n:-0}" = 1 ] && break
	sleep 0.2; i=$((i+1))
done

for k in $(seq 1 50); do cli set 0 k$k v$k; done
for k in $(seq 1 20); do cli get 0 k$k; done
cli get 0 nosuchkey

python3 - "$CLI" <<'PY_EOF'
import json, subprocess, sys, time, urllib.request
CLI = sys.argv[1]
npass = nfail = 0
def ok(m):
    global npass; npass += 1; print("  ok   " + m)
def bad(m):
    global nfail; nfail += 1; print("  FAIL " + m)
def stats(port=17952):
    return json.load(urllib.request.urlopen("http://127.0.0.1:%d/stats" % port, timeout=5))
def cli(*a):
    subprocess.run([CLI, "-q", "-h", "127.0.0.1", "-p", "17951"] + list(a), capture_output=True, timeout=10)
def get(d, path):
    for p in path.split("."):
        if isinstance(d, list):
            d = d[int(p)]
        elif isinstance(d, dict) and p in d:
            d = d[p]
        else:
            return "MISSING"
    return d

s = stats(); t0 = time.time()
# 1. every window the page reads is published
paths = ["since.win_s", "process.win_cpu_user_ms", "process.win_cpu_sys_ms", "memory.win_nomem",
         "clients.win_refused", "native.binary.win_conns", "native.binary.win_requests",
         "native.resp.win_requests", "resp.win_conns", "resp.win_requests", "resp.win_rejected", "resp.win_authfail",
         "collections.0.win_hits_client", "collections.0.win_misses_client", "collections.0.win_stores_client",
         "collections.0.win_stores", "collections.0.win_expired", "collections.0.fleet.win_hits",
         "collections.0.fleet.win_misses", "collections.0.fleet.win_stores", "collections.0.fleet.win_expired",
         "cluster.win_pull_sent", "cluster.win_pull_hits", "cluster.win_pull_misses", "cluster.win_pull_timeouts",
         "cluster.win_pull_served", "cluster.win_fwd_sent", "cluster.win_fwd_served", "cluster.win_fwd_no_route",
         "cluster.win_fwd_send_fail", "cluster.win_repl_pushed", "cluster.win_repl_out",
         "cluster.win_repl_skipped_dying", "cluster.win_migrated_in", "cluster.win_migrated_out",
         "cluster.win_migrate_lost", "cluster.win_rx_applied", "cluster.win_rx_drops", "cluster.win_neg_hits",
         "cluster.win_tomb_sent", "cluster.win_tomb_applied", "cluster.win_tomb_displaced",
         "cluster.win_pend_exhausted", "cluster.win_pend_peak", "cluster.win_bad_auth",
         "cluster.win_lamport_rejected", "cluster.win_term_rejected", "cluster.map.win_refused",
         "wal.win_appended", "wal.win_bytes", "wal.win_dropped", "wal.win_late", "wal.win_overruns", "wal.win_heals",
         "wal.observed.win_fsync_n", "wal.observed.win_fsync_sum_us", "wal.observed.win_fsync_max_us",
         "rdb.win_saves", "unknown_commands.win_other", "unknown_commands.win_preauth"]
if s.get("pubsub"):
    paths += ["pubsub.win_published", "pubsub.win_delivered", "pubsub.win_relay_sent", "pubsub.win_relay_recv"]
miss = [p for p in paths if get(s, p) == "MISSING"]
(ok if not miss else bad)("all %d windows the page reads are in /stats (missing: %s)" % (len(paths), miss or "none"))
# 2. a young daemon's window is its whole count
c = s["collections"][0]
(ok if (c.get("win_stores_client"), c.get("win_hits_client"), c.get("win_misses_client")) == (50, 20, 1)
    and (c.get("stores_client"), c.get("hits_client"), c.get("misses_client")) == (50, 20, 1) else bad)(
    "younger than five minutes, a window is the whole count: stores/hits/misses %s / %s / %s"
    % (c.get("win_stores_client"), c.get("win_hits_client"), c.get("win_misses_client")))
ws = get(s, "since.win_s"); up = get(s, "process.uptime_s")
(ok if isinstance(ws, int) and 0 <= ws <= (up if isinstance(up, int) else ws) + 2 else bad)(
    "since.win_s reaches back over the uptime while that is under five minutes (%s s, up %s s)" % (ws, up))
# 3. the fleet: no member has a minute of baseline yet - null, not a few seconds
fl = c.get("fleet") or {}
(ok if fl.get("members") == 2 and fl.get("win_stores", "x") is None else bad)(
    "a fleet window with a member seen for seconds is null, not a guess (members %s, win_stores %r)"
    % (fl.get("members"), fl.get("win_stores", "absent")))
# 4. reset stats: the window counts from the reset
urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:17952/reset-stats", data=b"", method="POST"), timeout=5)
for k in range(7):
    cli("set", "0", "r%d" % k, "v")
c = stats()["collections"][0]
(ok if c.get("stores_client") == 7 and c.get("win_stores_client") == 7 else bad)(
    "after reset stats the window counts from the reset: 7 stores, window 7 (got %s, window %s)"
    % (c.get("stores_client"), c.get("win_stores_client")))
# 5. a minute later every member has a baseline: the fleet window is a figure
time.sleep(max(0, 66 - (time.time() - t0)))
for k in range(5):
    cli("set", "0", "late%d" % k, "v")
fl = stats()["collections"][0].get("fleet") or {}
w = fl.get("win_stores")
(ok if w == 12 else bad)(
    "a minute on, the fleet window is a figure: the 12 client writes since the reset (7 + 5) - "
    "a reset's baseline is exactly zero (%r; fleet total %s)" % (w, fl.get("stores")))
print("winstatstest: %d passed, %d failed" % (npass, nfail))
sys.exit(1 if nfail else 0)
PY_EOF
