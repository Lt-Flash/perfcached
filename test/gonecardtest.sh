#!/bin/sh
# gonecardtest.sh - S192: a member that LEAVES keeps its place in the
# fleet view for a couple of minutes.
#
# A node that shuts down cleanly sends GOODBYE and its peers purge it:
# the slot's id is zeroed, so it fell out of /members entirely and its
# card vanished from every other node's page.  The operator saw it
# during a rolling deploy - "one node disappeared and there was no
# backup" - and a fleet of three silently reading as two is the worst
# possible way to show a restart.  The id is reserved for a week; the
# CARD is kept for two minutes, which is what a restart takes.
#
# Fail-first: without S192 the departed node is absent from /members and
# the first assertion below fails on a KeyError.
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcgone.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

node() { # node <n> <tcp> <http>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = g-client-secret
cluster = g-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = 239.255.77.52:17152
advertise = 127.0.1.3$1
[collection 0]
buckets_log2 = 12
mode = eager
CONF
}
node 1 17720 17721
node 2 17722 17723
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 & P1=$!
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 & P2=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null &&
		grep -q "perfcached ready" "$D/n2.log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n1.log" || { echo "node 1 did not start"; cat "$D/n1.log"; exit 1; }

# node 2 leaves the way a deploy makes it leave: SIGTERM, so it says
# goodbye rather than simply stopping answering
python3 - "$P2" <<'PY_EOF'
import json, os, signal, sys, time, urllib.request

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)
def members():
    d = json.loads(urllib.request.urlopen(
        "http://127.0.0.1:17721/members", timeout=8).read())
    return {m["node"]: m for m in d.get("members", [])}

end = time.time() + 40
while time.time() < end and len(members()) < 2:
    time.sleep(0.5)
mm = members()
(ok if len(mm) == 2 else bad)("both members are in the fleet view (%d)" % len(mm))
peer = [n for n, m in mm.items() if not m.get("self")]
if not peer:
    bad("no peer in the view at all"); print("gonecardtest: %d passed, %d failed" % (pass_n, fail_n)); sys.exit(1)
peer = peer[0]

os.kill(int(sys.argv[1]), signal.SIGTERM)
end = time.time() + 30
while time.time() < end:
    m = members().get(peer)
    if m and m.get("state") == "gone":
        break
    time.sleep(0.5)

m = members().get(peer)
(ok if m else bad)("the departed member is STILL in the fleet view - a restart "
                   "must not read as a fleet that shrank")
if m:
    (ok if m.get("state") == "gone" else bad)(
        "and reads as gone, not as a lifecycle state it is not in (%r)" % m.get("state"))
    (ok if m.get("gone_s", -1) >= 0 else bad)(
        "with how long ago it left (%s s)" % m.get("gone_s"))
    (ok if m.get("node") == peer else bad)("under its own id, which is held for it")
    # the card's lifetime is the RESERVATION's, not a display timer: it
    # goes when nothing is holding the id any more, which is the only
    # honest moment for it to go
    held = m.get("held_s", 0)
    (ok if held > 600000 else bad)(
        "and says how much longer the id is held - a week, not a made-up "
        "window (%s s)" % held)

# S193: and every state a node CAN be in carries its reason, not just
# the one failure that had a caller remember to set it.  A card that
# says "recovering" and nothing else is the same complaint S186 fixed
# for FAILED.
st = json.loads(urllib.request.urlopen(
    "http://127.0.0.1:17721/stats", timeout=8).read())
(ok if "state_reason" in st else bad)("the node publishes its state_reason")
# stated as an implication a reader can check: the pair must be
# consistent, and the assertion prints BOTH so a pass cannot hide which
# branch it took
(ok if (st.get("state") == "ready") == (st.get("state_reason") == "") else bad)(
    "state and reason agree: state=%r reason=%r"
    % (st.get("state"), st.get("state_reason")))

print("gonecardtest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PY_EOF
rc=$?
exit $rc
