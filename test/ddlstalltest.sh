#!/bin/sh
# ddlstalltest.sh - a collection change from the fleet is written to disk off the cluster thread, and
# only when it changes the file: with that fsync held 2.5 s, reads and writes routed between nodes
# keep answering within the pull timeout.  Usage: test/ddlstalltest.sh [./perfcached] [./syncfailshim.so]
# Last, a runtime create with that fsync held: the creator's keepalives meanwhile carry its new set.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
[ -f "$SHIM" ] || { echo "ddlstalltest: $SHIM is missing (make syncfailshim.so)" >&2; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
finish() { echo "ddlstalltest: $pass passed, $fail failed"; [ $fail -eq 0 ]; exit; }
for p in 23641 23642 23643 23651 23652 23653; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "ddlstalltest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcds.XXXXXX)

conf() { # conf <n>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = ds-client-secret
cluster = ds-cluster-secret
enable = ds-enable-secret
[listen]
tcp = 127.0.0.1:2364$1
http = 127.0.0.1:2365$1
plaintext = loopback
[cluster]
multicast = 239.255.78.61:23669
advertise = 127.0.236.$1
pull_timeout_ms = 400
mode = shard
collections = c
[collection c]
buckets_log2 = 12
autoscale = off
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n>: under the shim, its classes armed by $D/ctl
	: > "$D/n$1.log"
	LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/sync.log" \
		PC_SYNCFAIL_STATE_MS=2500 "$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
st() { curl -s "http://127.0.0.1:2365$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')$(st 2 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')$(st 3 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# priv <n> <method> <params json>: enabled, one connection; prints the reply
priv() { python3 - "2364$1" "$2" "$3" <<'PY'
import json, pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30))
for i, (m, p) in enumerate([("enable", {"secret": "ds-enable-secret"}), (sys.argv[2], json.loads(sys.argv[3]))]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)
PY
}
# routed <n> <secs>: for <secs>, through node <n>, reads a000-a299 and writes 20 new keys a round;
# prints rounds, reads, reads wrong, writes, writes acknowledged
routed() { python3 - "2364$1" "$2" <<'PY'
import json, pcnative, socket, sys, time
port, secs = int(sys.argv[1]), float(sys.argv[2])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=60))
def batch(rq):
    f.write(("\n".join(json.dumps(x) for x in rq) + "\n").encode()); f.flush()
    out = {}
    for _ in rq:
        m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m)
        out[m["id"]] = m.get("result") or {"_err": m.get("error")}
    return out
rounds = reads = wrong = writes = acked = 0; t0 = time.time(); first = None
while time.time() - t0 < secs:
    g = batch([{"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "a%03d" % i}} for i in range(300)])
    bad = [i for i in range(300) if g[i].get("value") != "v1-a%03d" % i]
    reads += 300; wrong += len(bad)
    if bad and first is None:
        first = "a%03d at %.1f s: %s" % (bad[0], time.time() - t0, json.dumps(g[bad[0]])[:200])
    s = batch([{"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "w%d-%d" % (rounds, i), "value": "x"}} for i in range(20)])
    writes += 20; acked += sum(1 for r in s.values() if r.get("stored"))
    rounds += 1
print(rounds, reads, wrong, writes, acked)
if first:
    sys.stderr.write("  ..   first wrong read: %s\n" % first)
PY
}

for n in 1 2 3; do conf $n; done
: > "$D/ctl"
for n in 1 2 3; do start $n || { bad "node $n did not start"; finish; }; done
ready3 || { bad "the fleet did not form"; finish; }
W=$(python3 - <<'PY'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 23641), timeout=60))
f.write(("\n".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "a%03d" % i, "value": "v1-a%03d" % i}}) for i in range(300)) + "\n").encode()); f.flush()
n = 0
for _ in range(300):
    m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m)
    n += 1 if (m.get("result") or {}).get("stored") else 0
print(n)
PY
)
[ "$W" = 300 ] || bad "stored $W of 300 through node 1"

# the control: nothing armed, the same rounds
read -r R0 N0 X0 S0 A0 <<E
$(routed 2 3)
E
[ "${N0:-0}" -gt 0 ] && [ "$X0" = 0 ] && [ "$S0" = "$A0" ] \
	&& ok "control: $R0 rounds through node 2 - all $N0 reads right, $A0 of $S0 writes acknowledged" \
	|| bad "control: $N0 reads, $X0 wrong, $A0 of $S0 writes acknowledged"

echo stateslow > "$D/ctl"
R=$(priv 1 autoscale '{"col": "c", "mode": "warn"}')
case "$R" in *'"error"'*) bad "autoscale c warn refused: $R";; esac
R=$(priv 1 resize '{"col": "c", "buckets_log2": 14}')
case "$R" in *'"resizing":true'*) ;; *) bad "resize c refused: $R";; esac
# a mode the file must keep, then a resize announced four times that it need not: every
# collections-file fsync either brings is held 2.5 s
read -r R1 N1 X1 S1 A1 <<E
$(routed 2 12)
E
: > "$D/ctl"
[ "${N1:-0}" -gt 0 ] && [ "$X1" = 0 ] && [ "$S1" = "$A1" ] \
	&& ok "with the state file's fsync held 2.5 s: $R1 rounds through node 2 - all $N1 reads right, $A1 of $S1 writes acknowledged" \
	|| bad "with the state file's fsync held 2.5 s: $N1 reads, $X1 wrong, $A1 of $S1 writes acknowledged (pull timeout 400 ms)"

# the cause, from the shim's notes: which thread waited on which file
C2=$(grep -c "^stateslow .* $D/s2/collections.tmp" "$D/sync.log" 2>/dev/null)
C3=$(grep -c "^stateslow .* $D/s3/collections.tmp" "$D/sync.log" 2>/dev/null)
CL=$(grep -c "^stateslow pc-cluster " "$D/sync.log" 2>/dev/null)
[ "${C2:-0}" -ge 1 ] && [ "${C3:-0}" -ge 1 ] \
	&& ok "the fleet's autoscale change reached nodes 2 and 3's files under the hold ($C2 / $C3 held syncs)" \
	|| bad "nodes 2 and 3 synced their collections file ${C2:-0} / ${C3:-0} times under the hold - the stall was never reached"
[ "${CL:-0}" = 0 ] && ok "no held sync was the cluster thread's" \
	|| bad "the cluster thread waited on $CL held syncs: $(grep '^stateslow pc-cluster ' "$D/sync.log" | head -2 | sed "s|$D/||g" | tr '\n' ';')"
[ "${C2:-0}" -le 1 ] && [ "${C3:-0}" -le 1 ] \
	&& ok "one write a node for one change: the resize's four announces left the file alone" \
	|| bad "nodes 2 and 3 wrote their collections file $C2 / $C3 times for one change"
[ $fail -eq 0 ] || sed "s|$D/||g" "$D/sync.log" 2>/dev/null | sort | uniq -c | head -12 | sed 's/^/        /'
# a runtime create with the creator's collections-file fsync held 2.5 s: its keepalives go on
# meanwhile, and the fleet may neither refuse the creator nor split from it
echo stateslow > "$D/ctl"
R=$(priv 1 create '{"col": "d", "buckets_log2": 8}')
case "$R" in *'"created":true'*) ;; *) bad "create d refused: $R";; esac
read -r R2 N2 X2 S2 A2 <<E
$(routed 2 6)
E
: > "$D/ctl"
RF=$(grep -h "REFUSING peer" "$D"/n1.log "$D"/n2.log "$D"/n3.log 2>/dev/null | head -2 | cut -c1-160)
[ -z "$RF" ] && ok "no node refused another across the runtime create" \
	|| bad "the runtime create split the fleet: $(echo "$RF" | tr '\n' '|')"
[ "${N2:-0}" -gt 0 ] && [ "$X2" = 0 ] && [ "$S2" = "$A2" ] \
	&& ok "across the create: $R2 rounds through node 2 - all $N2 reads right, $A2 of $S2 writes acknowledged" \
	|| bad "across the create: $N2 reads, $X2 wrong, $A2 of $S2 writes acknowledged"
ready3 && ok "after the create the three nodes are ready with both peers up" \
	|| bad "after the create the fleet is not whole: up peers $(st 1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')/$(st 2 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')/$(st 3 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')"
HD=$(for n in 1 2 3; do st $n 'int(any(c["name"] == "d" for c in r["collections"]))'; done | tr -d '\n')
[ "$HD" = 111 ] && ok "every node holds the created collection" \
	|| bad "the created collection reached nodes 1/2/3: $HD"
finish
