#!/bin/sh
# stagerelaytest.sh - UP6n (0.6.5, DESIGN 12nf): a build staged on one node
# reaches the members, and each verifies it itself.
# Three eager nodes.  Nodes 1 and 2 trust a test key ([upgrade]
# trusted_keys); node 3 trusts only the release key built in.
#  1. a build signed by the test key, uploaded to node 1: staged there,
#     and the end says it went to 2 members;
#  2. node 2 stages it byte for byte, its own check naming the signer;
#  3. node 3 refuses it - it verifies with ITS keys, not node 1's word -
#     and stages nothing;
#  4. node 1's upgrade_status relay names node 2 staged, node 3 refused;
#  5. a member does not relay a build it received (no loop): node 2's
#     relay table stays empty;
#  6. UPS's upgrade fact: node 1's members name node 2 "staged" with the
#     build's first four hex digits, node 3 "idle" with none, and every
#     helper "absent" (none is installed on a test host);
#  7. M_UPG_SWITCH / M_UPG_ACK: node 1 asks node 2 to install it - node 2
#     holds it but has no helper, and says so; asks node 3 - not staged
#     there, and it says so.  (A member's ask never forces; the accepted
#     path is switchtest's 8.)
#  8. facts are never compared (UP1): the members' upgrade facts differ
#     now - node 2 staged, node 3 idle - and no node raises a config.*
#     alert over it (tombstonetest F covers equal facts; this, unequal).
# FAIL-FIRST: 0.6.4 has no upgrade_stage - nothing stages anywhere.
# Usage: test/stagerelaytest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsrel.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18070
command -v openssl > /dev/null || { echo "stagerelaytest: openssl is needed"; exit 1; }
openssl genpkey -algorithm ed25519 -out "$D/k.pem" 2> /dev/null
PUB=$(openssl pkey -in "$D/k.pem" -pubout -outform DER 2> /dev/null | tail -c 32 | od -An -tx1 | tr -d ' \n')

conf() { # conf <n> [upgrade lines]
	n=$1; shift
	cat > "$D/n$n.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$n
[memory]
arena_mb = 16
[secrets]
client = sr-client-secret
cluster = sr-cluster-secret
enable = sr-enable
[listen]
tcp = 127.0.0.1:$((18080 + n))
http = 127.0.0.1:$((18093 + n))
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.79.1,127.0.79.2,127.0.79.3
expect = 3
advertise = 127.0.79.$n
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
[upgrade]
EOF
	for l in "$@"; do printf '%s\n' "$l" >> "$D/n$n.conf"; done
	chmod 600 "$D/n$n.conf"; mkdir -p "$D/s$n"
}
st() { # st <n> <python expression over r (/stats)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18093 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
ustat() { # ustat <n> <python expression over s (upgrade_status)>
	python3 - "$1" "$2" <<'PY' 2>&1
import socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 18080 + int(sys.argv[1])), 10))
s = pcnative.call(f, "upgrade_status")["result"]
print(eval(sys.argv[2]))
PY
}

conf 1 "trusted_keys = $PUB"; conf 2 "trusted_keys = $PUB"; conf 3
for n in 1 2 3; do "$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 & echo $! > "$D/n$n.pid"; done
i=0; while [ $i -lt 120 ]; do
	[ "$(st 1 'r["cluster"]["peers_up"]')" = 2 ] && [ "$(st 2 'r["cluster"]["peers_up"]')" = 2 ] && [ "$(st 3 'r["cluster"]["peers_up"]')" = 2 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$(st 1 'r["cluster"]["peers_up"]')" = 2 ] || { bad "the fleet did not form: peers_up $(st 1 'r["cluster"]["peers_up"]')/$(st 2 'r["cluster"]["peers_up"]')/$(st 3 'r["cluster"]["peers_up"]')"; echo "stagerelaytest: $pass passed, $fail failed"; exit 1; }
sleep 3                                       # the members' facts ride their next ALIVE

head -c 1500000 /dev/urandom > "$D/a.tar.gz"
openssl pkeyutl -sign -inkey "$D/k.pem" -rawin -in "$D/a.tar.gz" -out "$D/a.tar.gz.sig" 2> /dev/null
SHA=$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$D/a.tar.gz")
r=$(python3 - "$D/a.tar.gz" <<'PY' 2>&1
import base64, hashlib, json, socket, sys
import pcnative
data = open(sys.argv[1], "rb").read()
sig = open(sys.argv[1] + ".sig", "rb").read().hex()
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 18081), 10))
pcnative.call(f, "enable", secret="sr-enable")
def call(**kw):
    r = pcnative.call(f, "upgrade_stage", **kw)
    if "error" in r:
        print(json.dumps({"error": r["error"].get("message"), "op": kw.get("op")}))
        sys.exit(1)
    return r["result"]
uid = call(op="begin", size=len(data), sha256=hashlib.sha256(data).hexdigest(), sig=sig, version="0.6.5")["id"]
off = 0
while off < len(data):
    off = call(op="chunk", id=uid, off=off, data=base64.b64encode(data[off:off + 200000]).decode())["got"]
print(json.dumps(call(op="end", id=uid)))
PY
)
echo "$r" | grep -q "\"sha256\": \"$SHA\"" && echo "$r" | grep -q '"relayed_to": 2' && echo "$r" | grep -q '"older_members": 0' &&
	ok "1. staged on node 1 and offered to both members ($r)" || bad "1. upload: $r"

i=0; while [ $i -lt 60 ]; do
	[ "$(ustat 1 'len(s["relay"]) == 2 and all(e["status"] != "sending" for e in s["relay"])')" = True ] && break
	sleep 0.5; i=$((i+1))
done
cmp -s "$D/a.tar.gz" "$D/s2/upgrade/staged/$SHA.tar.gz" && cmp -s "$D/a.tar.gz.sig" "$D/s2/upgrade/staged/$SHA.tar.gz.sig" &&
	[ "$(ustat 2 '[(e["sha256"] == "'"$SHA"'", e["signer"][:15]) for e in s["staged"]]')" = "[(True, 'trusted_keys[0]')]" ] &&
	ok "2. node 2 staged it byte for byte, its own check naming trusted_keys[0]" ||
	bad "2. node 2: $(ls "$D/s2/upgrade/staged" 2>&1 | tr '\n' ' '); status $(ustat 2 's["staged"]')"
[ ! -e "$D/s3/upgrade/staged/$SHA.tar.gz" ] && [ "$(ustat 3 's["staged"], s["refused_total"]')" = "([], 1)" ] &&
	grep -q "refused here: the signature does not verify" "$D/n3.log" &&
	ok "3. node 3, trusting only the release key, refused it and staged nothing" ||
	bad "3. node 3: staged $(ls "$D/s3/upgrade/staged" 2>&1 | tr '\n' ' '); status $(ustat 3 '(s["staged"], s["refused_total"])'); log $(grep -m2 upgrade "$D/n3.log" | tr '\n' '|')"
R=$(ustat 1 'sorted((e["member"], e["status"]) for e in s["relay"])')
[ "$R" = "[('127.0.79.2', 'staged'), ('127.0.79.3', 'refused: sha256 or signature')]" ] &&
	ok "4. node 1's relay: node 2 staged, node 3 refused" || bad "4. node 1's relay: $R"
[ "$(ustat 2 's["relay"]')" = "[]" ] && [ "$(ustat 3 's["relay"]')" = "[]" ] &&
	ok "5. the members relayed nothing on - a received build goes no further" || bad "5. relay on a member: 2 $(ustat 2 's["relay"]'), 3 $(ustat 3 's["relay"]')"
M=""
i=0; while [ $i -lt 30 ]; do
	M=$(python3 - <<'PY' 2>&1
import socket
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 18081), 10))
r = pcnative.call(f, "members")["result"]
m = r.get("members", []) if isinstance(r, dict) else r
print(sorted((x.get("advertise") or x.get("addr"), (x.get("upgrade") or {}).get("state"), (x.get("upgrade") or {}).get("staged"), (x.get("upgrade") or {}).get("helper")) for x in m))
PY
)
	# all three before judging: each node refreshes its own fact on its
	# own 5-s phase (fact_tick), so node 2's staged fact can reach node 1
	# before node 1's own tick has run - waiting on node 2's alone failed
	# GitHub's check-asan once (node 1 still idle)
	echo "$M" | grep -q -F "('127.0.79.2', 'staged', '$(echo "$SHA" | cut -c 1-4)', 'absent')" &&
		echo "$M" | grep -q -F "('127.0.79.1', 'staged', '$(echo "$SHA" | cut -c 1-4)', 'absent')" &&
		echo "$M" | grep -q -F "('127.0.79.3', 'idle', None, 'absent')" && break
	sleep 0.5; i=$((i+1))
done
# each member's own tuple - node 1's entry (staged too) must not stand in for node 2's
echo "$M" | grep -q -F "('127.0.79.2', 'staged', '$(echo "$SHA" | cut -c 1-4)', 'absent')" &&
	echo "$M" | grep -q -F "('127.0.79.3', 'idle', None, 'absent')" &&
	echo "$M" | grep -q -F "('127.0.79.1', 'staged', '$(echo "$SHA" | cut -c 1-4)', 'absent')" &&
	ok "6. UPS fact: node 2 staged $(echo "$SHA" | cut -c 1-4), node 3 idle, helpers absent - in node 1's members ($M)" ||
	bad "6. members' upgrade: $M"
ask() { # ask <member> - node 1 asks it to install the build, then waits for the answer
	python3 - "$1" "$SHA" <<'PY' 2>&1
import socket, sys, time
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 18081), 10))
pcnative.call(f, "enable", secret="sr-enable")
r = pcnative.call(f, "upgrade_switch", sha256=sys.argv[2], member=sys.argv[1])
if "error" in r:
    print("ERR", r["error"].get("message")); sys.exit(0)
for _ in range(40):
    s = pcnative.call(f, "upgrade_status")["result"]
    a = [x for x in s.get("asked", []) if x["member"] == sys.argv[1]]
    if a and not a[0]["status"].startswith("sent"):
        print(a[0]["status"], a[0]["staged"]); sys.exit(0)
    time.sleep(0.25)
print("no answer", a)
PY
}
r=$(ask 127.0.79.2)
[ "$r" = "refused: no helper enabled there $(echo "$SHA" | cut -c 1-8)" ] && grep -q "asked this node to install $SHA - refused: the upgrade helper is not enabled" "$D/n2.log" &&
	ok "7. node 1 asks node 2: it holds the build, has no helper, and answers so" || bad "7. ask node 2: '$r'; node 2 log: $(grep -m2 'asked this node' "$D/n2.log" | tr '\n' '|')"
r=$(ask 127.0.79.3)
[ "$r" = "refused: not staged there $(echo "$SHA" | cut -c 1-8)" ] &&
	ok "7. node 1 asks node 3, which refused the build: not staged there, it answers" || bad "7. ask node 3: '$r'"
CA=""
for n in 1 2 3; do CA="$CA $(st $n '[a["key"] for a in r["alerts"]["active"] if a["key"].startswith("config.")]')"; done
[ "$CA" = " [] [] []" ] && ok "8. the members' upgrade facts differ, and no node calls it a config difference" ||
	bad "8. config alerts over unequal facts: $CA"
E=0
for n in 1 2 3; do
	grep -q -E 'ERROR|CRIT' "$D/n$n.log" && { E=1; bad "node $n logged: $(grep -m2 -E 'ERROR|CRIT' "$D/n$n.log" | tr '\n' '|')"; }
done
[ $E = 0 ] && ok "no ERROR or CRIT on any node"

echo "stagerelaytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
