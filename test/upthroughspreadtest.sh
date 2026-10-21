#!/bin/sh
# upthroughspreadtest.sh - DESIGN 12nt, TC-3 (0.7.1): a through collection in
# SPREAD mode (K holders of P nodes) fetches and holds at the key's holders.
#   - four spread members (replicas = 2), collection 0 read-through +
#     write-through on the fakes (M, R1, R2 of upwritethroughtest), 30 keys
#     seeded straight into the master;
#   - every key reads through node 1 with its value: a key node 1 does not
#     hold is pulled from a holder, which fetches it and keeps the copy -
#     30 fetches in all, every copy on the node that fetched it, no node
#     holding them all;
#   - the same keys through node 4: found; a holder asked for the first time
#     fetches once more at most (K copies at most, never P);
#   - a change on the master reaches the holders' copies (tracking);
#   - an absent key: one fetch walk per holder asked, the holder's negative
#     marker answers the next pull;
#   - a write through node 1 goes to the master once and its read-back is
#     pushed to the K holders (marked copies; the write's own invalidation
#     drops them again, S404); SET NX twice through node 2: stored once,
#     refused once; a change on the master reads back everywhere;
#   - a node restarted comes back ready and reads every key; a change on
#     the master still reaches every node;
#   - the drain sweep on every node takes over its holder share only (TC-4).
# Usage: test/upthroughspreadtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcupsp.XXXXXX)
PM=19941 P1=19942 P2=19943
FM= F1= F2= P1ID= P2ID= P3ID= P4ID=
trap 'for p in $P1ID $P2ID $P3ID $P4ID $FM $F1 $F2; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $P2 19951 19952 19953 19954 19961 19962 19963 19964; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upthroughspreadtest: port $p already bound" >&2; exit 1; }
done
ctl() { echo "$2" > "$D/up$1/ctl.json.tmp" && mv "$D/up$1/ctl.json.tmp" "$D/up$1/ctl.json"; }
for n in m 1 2; do mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"; done
ctl m '{"role": "master", "accept_writes": true, "replicate_to": ["'"$D"'/up1", "'"$D"'/up2"]}'
ctl 1 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
ctl 2 '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}'
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
python3 "$T/fakeupstream.py" $P2 "$D/up2" > "$D/f2.log" 2>&1 & F2=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ] && [ -f "$D/up2/ready" ]; }; do sleep 0.1; i=$((i+1)); done
rawm() { python3 - $PM "$@" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
a = [x.encode() for x in sys.argv[2:]]
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
print(s.makefile("rb").readline().decode("latin-1").strip())
PY
}
node() { # node <n>
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = upsh-client-secret
cluster = upsh-cluster-secret
enable = upsp-enable
[listen]
tcp = 127.0.0.1:1995$1
http = 127.0.0.1:1996$1
plaintext = loopback
[cluster]
multicast = 239.255.78.25:19944
advertise = 127.0.85.$1
mode = spread
replicas = 2
collections = 0
pull_timeout_ms = 200
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$P2, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_negative_ms = 5000
C
	chmod 600 "$D/n$1.conf"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
st() { # st <n> <python expr over s (the node's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:1996%s/stats" % sys.argv[1], timeout=5))
print(eval(sys.argv[2]))
PY
}
col0() { st "$1" "[c for c in s['collections'] if c['name']=='0'][0]$2"; }
nget() { # nget <n> <key> -> the value, or None
	python3 - "$1" "$2" <<'PY' 2>&1
import socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19950 + int(sys.argv[1])), 10))
r = pcnative.call(f, "get", col="0", key=sys.argv[2])
e = r.get("error")
print("ERR " + str(e) if e else (r["result"].get("value") if r["result"].get("found") else None))
PY
}
for k in $(seq 1 30); do rawm SET "k:$k" "v$k" > /dev/null; done
[ "$(rawm GET k:30)" = '$3' ] && ok "30 keys seeded on the fake master (replicated to R1, R2)" || bad "seed: $(rawm GET k:30)"
node 1; node 2; node 3; node 4
i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']")" != 3 ]; do sleep 0.2; i=$((i+1)); done
[ "$(st 1 "s['cluster']['peers_up']")" = 3 ] && ok "four spread members up (K = 2)" || bad "peers_up on node 1: $(st 1 "s['cluster']['peers_up']")"
for n in 1 2 3 4; do i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done; done
sleep 1

# per-node figures: fetches / shadow_entries / entries / negative_hits / pull_fetching
fig() { # fig <stat path under collection 0 upstream> -> "n1 n2 n3 n4"
	python3 - "$1" <<'PY' 2>&1
import json, sys, urllib.request
o = []
for n in (1, 2, 3, 4):
    s = json.load(urllib.request.urlopen("http://127.0.0.1:1996%d/stats" % n, timeout=5))
    c = [c for c in s["collections"] if c["name"] == "0"][0]
    o.append(str(c["upstream"][sys.argv[1]] if sys.argv[1] != "entries" else c["entries"]))
print(" ".join(o))
PY
}
cl() { # cl <cluster counter> -> "n1 n2 n3 n4"
	python3 - "$1" <<'PY' 2>&1
import json, sys, urllib.request
print(" ".join(str(json.load(urllib.request.urlopen("http://127.0.0.1:1996%d/stats" % n, timeout=5))["cluster"][sys.argv[1]]) for n in (1, 2, 3, 4)))
PY
}
sum4() { python3 -c 'import sys; print(sum(int(x) for x in sys.argv[1].split()))' "$1"; }
max4() { python3 -c 'import sys; print(max(int(x) for x in sys.argv[1].split()))' "$1"; }
same() { [ "$1" = "$2" ]; }

# ---- 1. every key reads through node 1 with its value ------------------------------------
miss=0; for k in $(seq 1 30); do [ "$(nget 1 "k:$k")" = "v$k" ] || { miss=$((miss+1)); [ $miss -le 2 ] && echo "   k:$k via node 1: $(nget 1 "k:$k")"; }; done
[ $miss -eq 0 ] && ok "1. all 30 keys read through node 1 with their values" || bad "1. $miss of 30 wrong or absent through node 1"
F=$(fig fetches); SH=$(fig shadow_entries); PF=$(cl pull_fetching); PD=$(cl pull_fetched)
[ "$(sum4 "$F")" -eq 30 ] && ok "1. the fakes were fetched 30 times in all ($F) - once per key, at a holder" || bad "1. fetches $F (30 expected)"
same "$F" "$SH" && [ "$(max4 "$SH")" -lt 30 ] && ok "1. every copy sits on the node that fetched it: shadow_entries $SH, no node holds them all" || bad "1. shadow_entries $SH vs fetches $F"
[ "$(sum4 "$PF")" -gt 0 ] && same "$PF" "$PD" && ok "1. the holders fetched for node 1's pulls: pull_fetching $PF = pull_fetched $PD" || bad "1. pull_fetching $PF, pull_fetched $PD"

# ---- 2. the same keys through node 4: found; at most K copies -----------------------------
miss=0; for k in $(seq 1 30); do [ "$(nget 4 "k:$k")" = "v$k" ] || miss=$((miss+1)); done
XF2=$(fig fetches); SH2=$(fig shadow_entries)
[ $miss -eq 0 ] && [ "$(sum4 "$XF2")" -le 60 ] && same "$XF2" "$SH2" && ok "2. the same 30 keys through node 4: all found, $(sum4 "$XF2") fetches in all (at most K = 2 per key), copies on the fetching holders ($SH2)" || bad "2. miss $miss, fetches $F2, copies $SH2"

# ---- 3. a change on the master reaches the holders' copies --------------------------------
rawm SET k:5 changed > /dev/null
sleep 0.5
[ "$(nget 2 k:5)" = changed ] && [ "$(nget 3 k:5)" = changed ] && ok "3. k:5 changed on the master: read through nodes 2 and 3 is the new value (the holders' copies were invalidated and fetched again)" || bad "3. k:5 via node 2: $(nget 2 k:5), node 3: $(nget 3 k:5)"

# ---- 4. an absent key --------------------------------------------------------------------
XF3=$(fig fetches)
r=$(nget 1 k:nope); XF4=$(fig fetches)
r2=$(nget 2 k:nope); XF5=$(fig fetches)
d1=$(( $(sum4 "$XF4") - $(sum4 "$XF3") )); d2=$(( $(sum4 "$XF5") - $(sum4 "$XF4") ))
[ "$r" = None ] && [ "$r2" = None ] && ok "4. an absent key reads as absent through nodes 1 and 2" || bad "4. k:nope: $r / $r2"
[ $d1 -ge 1 ] && [ $d1 -le 3 ] && [ $d2 -le 3 ] && ok "4. the absent key cost one fetch walk per holder asked ($d1 then $d2 fetches), never one per read everywhere" || bad "4. fetches for the absent key: $d1 then $d2 ($F3 -> $F4 -> $F5)"

# ---- 5. a write through node 1 goes to the master and its read-back to the K holders -------
# The record Redis confirms is pushed to the K holders (each marks it a copy: copies_marked)
# and dropped again at once by the invalidation Redis sends for the write (S404) - so what
# stays observable is the push, the master's state and the reads, not the entries.
nset() { # nset <n> <verb> <key> <value> [ttl]
	python3 - "$@" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19950 + int(sys.argv[1])), 10))
kw = dict(col="0", key=sys.argv[3], value=sys.argv[4])
if len(sys.argv) > 5: kw["ttl"] = int(sys.argv[5])
r = pcnative.call(f, sys.argv[2], **kw)
print(json.dumps(r.get("result") if "result" in r else r, sort_keys=True))
PY
}
mdata() { # mdata <key> <field> - the fake master's stored entry field, or None
	python3 -c 'import json,sys
d=json.load(open(sys.argv[1]+"/data.json")); e=d.get(sys.argv[2])
print(None if e is None else e.get(sys.argv[3]))' "$D/upm" "$1" "$2" 2>/dev/null
}
cm() { # the marked copies (pushed write-through records) on nodes 1 2 3 4
	python3 - <<'PY' 2>&1
import json, urllib.request
o = []
for n in (1, 2, 3, 4):
    s = json.load(urllib.request.urlopen("http://127.0.0.1:1996%d/stats" % n, timeout=5))
    o.append(str([c for c in s["collections"] if c["name"] == "0"][0]["upstream"]["write_through"]["copies_marked"]))
print(" ".join(o))
PY
}
C0=$(cm)
r=$(nset 1 set w:1 first)
sleep 1
C1=$(cm)
echo "$r" | grep -q '"stored": true' && [ "$(grep -c '^SET w:1 first' "$D/upm/writes")" = 1 ] && [ "$(mdata w:1 value)" = first ] && ok "5. w:1 written through node 1: the master got the SET exactly once and holds the value" || bad "5. set: $r, master $(grep -c '^SET w:1' "$D/upm/writes") SET(s), value $(mdata w:1 value)"
dc=$(( $(sum4 "$C1") - $(sum4 "$C0") ))
[ $dc -ge 1 ] && [ $dc -le 2 ] && ok "5. the read-back was pushed to the holders, who marked it a copy ($dc marked: copies_marked $C0 -> $C1)" || bad "5. copies_marked $C0 -> $C1 (1 or 2 more expected: K = 2 holders, the writer may be one)"
[ "$(nget 2 w:1)" = first ] && [ "$(nget 3 w:1)" = first ] && [ "$(nget 4 w:1)" = first ] && ok "5. w:1 reads back through nodes 2, 3 and 4" || bad "5. w:1 via 2/3/4: $(nget 2 w:1) $(nget 3 w:1) $(nget 4 w:1)"

# ---- 6. SET NX twice through node 2 ----------------------------------------------------------
r=$(nset 2 setnx w:nx tokA 60); sleep 0.5; C2=$(cm)
r2=$(nset 2 setnx w:nx tokB 60); sleep 0.5; C3=$(cm)
echo "$r" | grep -q '"stored": true' && echo "$r2" | grep -q '"stored": false' && [ "$(mdata w:nx value)" = tokA ] && [ "$(grep -c '^SET w:nx' "$D/upm/writes")" = 2 ] && ok "6. SET NX through node 2: stored, then refused by the master (both reached it, tokA kept)" || bad "6. setnx: $r / $r2, master value $(mdata w:nx value), $(grep -c '^SET w:nx' "$D/upm/writes") SETs"
[ $(( $(sum4 "$C2") - $(sum4 "$C1") )) -ge 1 ] && same "$C2" "$C3" && ok "6. the stored NX was pushed to its holders, the refused one to nobody (copies_marked $C1 -> $C2 -> $C3)" || bad "6. copies_marked $C1 -> $C2 -> $C3"
[ "$(nget 3 w:nx)" = tokA ] && [ "$(nget 4 w:nx)" = tokA ] && ok "6. w:nx reads tokA through nodes 3 and 4" || bad "6. w:nx via 3/4: $(nget 3 w:nx) $(nget 4 w:nx)"

# ---- 7. a change on the master after a write ---------------------------------------------------
rawm SET w:1 changed > /dev/null
sleep 0.7
[ "$(nget 1 w:1)" = changed ] && [ "$(nget 4 w:1)" = changed ] && ok "7. w:1 changed on the master: nodes 1 and 4 read the new value" || bad "7. w:1 via node 1: $(nget 1 w:1), node 4: $(nget 4 w:1)"

# ---- 8. a node restarted ---------------------------------------------------------------------------
r=$(nset 3 set w:2 second); sleep 0.5
[ "$(nget 1 w:2)" = second ] || bad "8. w:2 via node 1 before the restart: $(nget 1 w:2)"
H=1
kill -9 $P1ID 2>/dev/null; sleep 0.3
"$BIN" -f "$D/n$H.conf" >> "$D/n$H.log" 2>&1 &
P1ID=$!
i=0; while [ $i -lt 150 ] && ! tail -40 "$D/n$H.log" | grep -q "perfcached ready"; do sleep 0.1; i=$((i+1)); done
i=0; while [ $i -lt 100 ] && [ "$(st $H "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done
sleep 1
echo "   after the restart of node $H: state $(st $H "s['state']"), shadow_entries $(fig shadow_entries), entries $(fig entries)"
[ "$(st $H "s['state']")" = ready ] && [ "$(nget $H w:2)" = second ] && [ "$(nget $H k:7)" = v7 ] && ok "8. the restarted node $H is ready and reads w:2 and k:7 (fetched again or pulled from a holder)" || bad "8. node $H after the restart: state $(st $H "s['state']"), w:2 $(nget $H w:2), k:7 $(nget $H k:7)"
rawm SET w:2 third > /dev/null
sleep 0.7
all=1; for n in 1 2 3 4; do [ "$(nget $n w:2)" = third ] || { all=0; echo "   w:2 via node $n: $(nget $n w:2)"; }; done
[ $all = 1 ] && ok "8. w:2 changed on the master after the restart: every node reads 'third' - no stale copy anywhere" || bad "8. a node still reads the old w:2 after the master changed it"

# ---- 9. the drain sweep takes over each node's share --------------------------------------
drain() { # drain <n> - updrain on node n (privileged), its result
	python3 - "$1" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 19950 + int(sys.argv[1])), 10))
pcnative.call(f, "enable", secret="upsp-enable")
r = pcnative.call(f, "updrain", col="0")
print(json.dumps(r.get("result", r.get("error")), sort_keys=True))
PY
}
dst() { col0 "$1" "['upstream']['drain']['$2']"; }
NR=$(python3 -c 'import json,sys,time; d=json.load(open(sys.argv[1])); print(sum(1 for k,e in d.items() if not e.get("exp") or e["exp"] > time.time()*1000))' "$D/upm/data.json")
for n in 1 2 3 4; do r=$(drain $n); echo "$r" | grep -q -i 'error' && echo "   updrain on node $n: $r"; done
i=0; while [ $i -lt 100 ] && { for n in 1 2 3 4; do [ "$(dst $n state)" = done ] || exit 1; done; } 2>/dev/null; [ $? -ne 0 ]; do sleep 0.2; i=$((i+1)); done
RD=; TK=; HD=; EL=; for n in 1 2 3 4; do RD="$RD $(dst $n read)"; TK="$TK $(dst $n taken_over)"; HD="$HD $(dst $n fleet_had)"; EL="$EL $(dst $n elsewhere)"; done
sum() { python3 -c 'import sys; print(sum(int(x) for x in sys.argv[1].split()))' "$1"; }
allv() { python3 -c 'import sys; print(all(int(x) == int(sys.argv[2]) for x in sys.argv[1].split()))' "$1" "$2"; }
[ "$(sum "$RD")" -eq $((NR * 2)) ] && ok "9. the drain sweeps read each of the master's $NR keys at its K = 2 holders (read:$RD)" || bad "9. drain read:$RD ($NR keys on the master, 2 x expected)"
[ $(( $(sum "$TK") + $(sum "$HD") )) -eq $((NR * 2)) ] && [ "$(sum "$EL")" -eq $((NR * 2)) ] && ok "9. each key taken (or already held) by its K = 2 holders - taken_over:$TK fleet_had:$HD, the other nodes skipped it (elsewhere:$EL)" || bad "9. taken_over:$TK fleet_had:$HD elsewhere:$EL (want 2 x $NR taken+had, 2 x $NR elsewhere)"
grep -q -E 'ERROR|CRIT' "$D"/n?.log && bad "the nodes logged: $(grep -h -m2 -E 'ERROR|CRIT' "$D"/n?.log | tr '\n' '|')" || ok "no ERROR or CRIT in the nodes' logs"
echo "upthroughspreadtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
