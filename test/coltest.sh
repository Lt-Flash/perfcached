#!/bin/sh
# coltest.sh - collections created, resized, renamed and dropped while the daemon serves, across a
# restart and a two-node fleet, and the RESP door's index-to-name map with its INFO keyspace.
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pccol.XXXXXX)
# j() dials once per call and privilege is per connection, so
# it raises `enable` on each link when this is set.
PC_ENABLE=col-enable-secret
export PC_ENABLE
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
# conf <id> <port> <allow_create yes|no> [cluster-advertise]
conf() {
	mkdir -p "$D/st$1"
	{
	printf '[daemon]\nworkers = 2\nlog_level = notice\nstate_dir = %s/st%s\nallow_create = %s\n' "$D" "$1" "$3"
	printf '[memory]\narena_mb = 32\n[secrets]\nclient = col-client-secret\nenable = col-enable-secret\ncluster = col-cluster-secret\n'
	printf '[listen]\ntcp = 127.0.0.1:%s\nresp = 127.0.0.1:%s\nplaintext = loopback\nresp_collections = 0:sbcha, 1:th, plain\n' "$2" "$(( $2 + 100 ))"
	[ -n "$4" ] && printf '[cluster]\nmulticast = 239.255.77.70:17170\nadvertise = %s\n' "$4"
	printf '[collection sbcha]\nbuckets_log2 = 8\n[collection th]\nbuckets_log2 = 8\n[collection plain]\nbuckets_log2 = 8\n[collection hidden]\nbuckets_log2 = 8\n'
	} > "$D/n$1.conf"
	chmod 640 "$D/n$1.conf"
}
start() { # start <id>
	# the log is truncated first: a restart that greps a ready line left
	# by the PREVIOUS start returns before this daemon is listening
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	eval "P$1=$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.log"; exit 1
}
stop() { eval "kill -9 \$P$1" 2>/dev/null; eval "wait \$P$1" 2>/dev/null; sleep 0.3; }
alive() { eval "kill -0 \$P$1" 2>/dev/null && echo yes || echo NO; }
# j <port> <method> [json params]
j() {
	python3 - "$@" <<'PYEOF'
import json, pcnative, socket, sys
port, method = int(sys.argv[1]), sys.argv[2]
params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
s = socket.create_connection(("127.0.0.1", port), timeout=20); f = pcnative.wrap(s)
# privilege is per connection and this helper dials once per call, so DDL needs enable on
# this link first; a daemon without an enable secret refuses it and the request still goes
import os
_es = os.environ.get("PC_ENABLE")
if _es:
    f.write((json.dumps({"jsonrpc": "2.0", "id": 0, "method": "enable",
        "params": {"secret": _es}}) + "\n").encode())
    f.flush(); f.readline()
r = {"jsonrpc": "2.0", "id": 1, "method": method}
if params is not None: r["params"] = params
f.write((json.dumps(r) + "\n").encode()); f.flush()
print(json.dumps(json.loads(f.readline())))
PYEOF
}
cols() { j "$1" collections | python3 -c "import json,sys; print(' '.join(sorted(c['name'] for c in json.load(sys.stdin)['result']['collections'])))"; }
# buckets of ONE collection on ONE node, or "gone" when it is not there
bkt() { j "$1" collections | python3 -c "
import json, sys
want = sys.argv[1]
for c in json.load(sys.stdin)['result']['collections']:
    if c['name'] == want:
        print(c['buckets']); break
else:
    print('gone')" "$2"; }
# jseq <port> <method> <json params> [<method> <json params>...] - on ONE connection, back to back
jseq() {
	python3 - "$@" <<'PYEOF'
import json, os, pcnative, socket, sys
port, a = int(sys.argv[1]), sys.argv[2:]
s = socket.create_connection(("127.0.0.1", port), timeout=20); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 0, "method": "enable",
    "params": {"secret": os.environ["PC_ENABLE"]}}) + "\n").encode()); f.flush(); f.readline()
for i in range(0, len(a), 2):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": a[i],
        "params": json.loads(a[i + 1])}) + "\n").encode())
f.flush()
for i in range(0, len(a), 2):
    print(f.readline().decode().strip())
PYEOF
}
# entries <port> <col>: the records it holds on that node, 0 when it is not there
# (by name off the collections list: a clustered node's stats carry other "entries" fields)
entries() { j "$1" collections | python3 -c "
import json, sys
print(sum(c['entries'] for c in json.load(sys.stdin)['result']['collections'] if c['name'] == sys.argv[1]))" "$2"; }
resp() { # resp <port> <cmd...>
	python3 - "$@" <<'PYEOF'
import socket, sys
port = int(sys.argv[1]); args = sys.argv[2:]
s = socket.create_connection(("127.0.0.1", port), timeout=20); f = s.makefile("rwb")
f.write(("*%d\r\n" % len(args) + "".join("$%d\r\n%s\r\n" % (len(a), a) for a in args)).encode()); f.flush()
l = f.readline().decode().strip()
if l.startswith("$") and l != "$-1":
    n = int(l[1:]); body = b""
    while len(body) < n + 2: body += f.read(n + 2 - len(body))
    print(body[:n].decode().replace("\r\n", " | ").strip())
else:
    print(l)
PYEOF
}

# ---- 1. the gate
conf 1 17751 no
start 1
R=$(j 17751 create '{"col":"sessions"}')
case "$R" in *"not enabled here"*) ok "with allow_create off a create is refused, and the message names the setting";; *) bad "gate off: $R";; esac
stop 1
conf 1 17751 yes
start 1
R=$(j 17751 create '{"col":"sessions"}')
case "$R" in *'"created": true'*'"buckets": 4096'*) ok "with allow_create on a collection is created at the default size (2^12)";; *) bad "create: $R";; esac
R=$(j 17751 create '{"col":"sessions"}')
case "$R" in *"already exists"*) ok "a duplicate name is refused";; *) bad "duplicate: $R";; esac
R=$(j 17751 create '{"col":"tiny","buckets_log2":6}')
case "$R" in *'"buckets": 64'*) ok "buckets_log2 sizes the table";; *) bad "sized create: $R";; esac
R=$(j 17751 create '{"col":"nope","buckets_log2":3}')
case "$R" in *"4..24"*) ok "an out-of-range size is refused";; *) bad "bad size: $R";; esac

# ---- 2. it is a collection like any other
j 17751 set '{"col":"sessions","key":"k1","value":"v1"}' > /dev/null
R=$(j 17751 get '{"col":"sessions","key":"k1"}')
case "$R" in *'"value": "v1"'*) ok "the created collection takes writes and serves reads";; *) bad "set/get: $R";; esac
[ "$(cols 17751)" = "hidden plain sbcha sessions th tiny" ] && ok "it is listed beside the configured ones" || bad "collections: $(cols 17751)"
R=$(j 17751 stats '{"col":"sessions"}')
case "$R" in *'"entries": 1'*) ok "stats reports it";; *) bad "stats: $R";; esac

# ---- 3. drop
R=$(j 17751 drop '{"col":"sessions"}')
case "$R" in *'"dropped": false'*'"entries": 1'*) ok "a non-empty collection is not dropped without force, and the reply says how many records stand in the way";; *) bad "drop: $R";; esac
R=$(j 17751 drop '{"col":"sessions","force":true}')
case "$R" in *'"dropped": true'*'"records": 1'*) ok "force drops it and counts the records freed";; *) bad "forced drop: $R";; esac
R=$(j 17751 get '{"col":"sessions","key":"k1"}')
case "$R" in *"no such collection"*) ok "every lookup after the drop says no such collection";; *) bad "get after drop: $R";; esac
[ "$(cols 17751)" = "hidden plain sbcha th tiny" ] && ok "it is gone from the list" || bad "after drop: $(cols 17751)"
R=$(j 17751 drop '{"col":"nothing"}')
case "$R" in *"no such collection"*) ok "dropping a name that does not exist is an error, not a silent success";; *) bad "drop missing: $R";; esac

# ---- 4. across a restart
stop 1
start 1
[ "$(cols 17751)" = "hidden plain sbcha th tiny" ] && ok "the restart brings back the created collection and leaves the dropped one dropped" || bad "after restart: $(cols 17751)"
grep -q "carried across the restart" "$D/n1.log" && ok "the daemon says so at startup" || bad "no restart line"
j 17751 set '{"col":"tiny","key":"t","value":"1"}' > /dev/null
R=$(j 17751 get '{"col":"tiny","key":"t"}')
case "$R" in *'"value": "1"'*) ok "the carried collection works after the restart";; *) bad "carried collection: $R";; esac

# ---- 4b. a drop frees EVERY record, and a re-create starts empty
python3 - <<'PYEOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 17751), timeout=30); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":{"col":"tiny","key":"bk%d"%i,"value":"v"}}) for i in range(5000)]
f.write(("\n".join(reqs)+"\n").encode()); f.flush()
for _ in reqs: f.readline()
PYEOF
E=$(j 17751 stats '{"col":"tiny"}' | sed -n 's/.*"entries": \([0-9]*\).*/\1/p' | head -1)
R=$(j 17751 drop '{"col":"tiny","force":true}')
case "$R" in *"\"records\": $E"*) ok "a drop frees every record it held ($E), not the fraction a single mutating walk reaches";; *) bad "drop freed: $R (the collection held $E)";; esac
j 17751 create '{"col":"tiny","buckets_log2":6}' > /dev/null
R=$(j 17751 stats '{"col":"tiny"}')
case "$R" in *'"entries": 0'*) ok "a collection re-created in the slot the drop left starts empty";; *) bad "re-create: $R";; esac

# ---- 4c. resize, both directions, while it serves
python3 - <<'PYEOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 17751), timeout=30); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":{"col":"plain","key":"pk%d"%i,"value":"v%d"%i,"ttl":600}}) for i in range(6000)]
f.write(("\n".join(reqs)+"\n").encode()); f.flush()
for _ in reqs: f.readline()
PYEOF
R=$(j 17751 resize '{"col":"plain","buckets_log2":16}')
case "$R" in *'"resizing": true'*) ok "a resize starts and returns at once, rather than holding the connection for the migration";; *) bad "resize: $R";; esac
# writes and a delete WHILE it migrates
j 17751 del '{"col":"plain","key":"pk3"}' > /dev/null
j 17751 set '{"col":"plain","key":"pk4","value":"CHANGED"}' > /dev/null
j 17751 set '{"col":"plain","key":"during","value":"new"}' > /dev/null
i=0
while [ $i -lt 120 ]; do
	R=$(j 17751 stats '{"col":"plain"}')
	case "$R" in *resizing_to*) sleep 0.5; i=$((i+1));; *) break;; esac
done
case "$R" in *'"buckets": 65536'*) ok "the collection is at its new size (after $((i / 2))s)";; *) bad "after resize: $R";; esac
case "$R" in *'"entries": 6000'*) ok "with every record carried across";; *) bad "entries after resize: $R";; esac
R=$(j 17751 get '{"col":"plain","key":"pk3"}')
case "$R" in *'"found": false'*) ok "a key deleted DURING the migration stays deleted - the copier had already carried it, and the swap did not bring it back";; *) bad "deleted during resize: $R";; esac
R=$(j 17751 get '{"col":"plain","key":"pk4"}')
case "$R" in *CHANGED*) ok "a key overwritten during the migration keeps the new value";; *) bad "overwritten during resize: $R";; esac
R=$(j 17751 get '{"col":"plain","key":"during"}')
case "$R" in *'"value": "new"'*) ok "a key written during the migration is there";; *) bad "written during resize: $R";; esac
R=$(j 17751 get '{"col":"plain","key":"pk5999"}')
case "$R" in *'"value": "v5999"'*) ok "and one written long before it, with its TTL";; *) bad "old key: $R";; esac
R=$(j 17751 resize '{"col":"plain","buckets_log2":4}')
case "$R" in *"grow it straight back"*) ok "a target the splitter would immediately undo is refused";; *) bad "too-small resize: $R";; esac
R=$(j 17751 resize '{"col":"plain","buckets_log2":16}')
case "$R" in *"already that size"*) ok "resizing to the size it already has is refused";; *) bad "same-size resize: $R";; esac
R=$(j 17751 resize '{"col":"nothing","buckets_log2":8}')
case "$R" in *"no such collection"*) ok "resizing a collection that does not exist is an error";; *) bad "resize missing: $R";; esac

# ---- 4d. rename: the atomic cutover a restore-into-a-second-collection wants
j 17751 create '{"col":"restored","buckets_log2":8}' > /dev/null
j 17751 set '{"col":"restored","key":"rk","value":"rv"}' > /dev/null
R=$(j 17751 rename '{"col":"restored","to":"live"}')
case "$R" in *'"renamed": true'*) ok "a collection is renamed while it serves";; *) bad "rename: $R";; esac
R=$(j 17751 get '{"col":"live","key":"rk"}')
case "$R" in *'"value": "rv"'*) ok "its records are reached under the new name, with no copy";; *) bad "read after rename: $R";; esac
R=$(j 17751 get '{"col":"restored","key":"rk"}')
case "$R" in *"no such collection"*) ok "and the old name is gone";; *) bad "old name after rename: $R";; esac
R=$(j 17751 rename '{"col":"live","to":"plain"}')
case "$R" in *"already exists"*) ok "a rename onto a name in use is refused";; *) bad "rename collision: $R";; esac
R=$(j 17751 rename '{"col":"nothing","to":"x"}')
case "$R" in *"no such collection"*) ok "renaming a collection that does not exist is an error";; *) bad "rename missing: $R";; esac

# ---- 4e. a create of a live name is refused even with a reusable dead slot ahead of it
j 17751 create '{"col":"sh_a","buckets_log2":6}' > /dev/null
j 17751 create '{"col":"sh_b","buckets_log2":6}' > /dev/null
j 17751 set '{"col":"sh_b","key":"k","value":"kept"}' > /dev/null
j 17751 drop '{"col":"sh_a"}' > /dev/null
sleep 3                                # the retire tick frees sh_a's slot for reuse
R=$(j 17751 create '{"col":"sh_b","buckets_log2":6}')
case "$R" in *"already exists"*) ok "a create of a live name behind a reusable dead slot is refused";; *) bad "duplicate behind a dead slot: $R";; esac
R=$(j 17751 get '{"col":"sh_b","key":"k"}')
case "$R" in *'"value": "kept"'*) ok "and the live collection is not shadowed by an empty one";; *) bad "shadowed: $R";; esac

# ---- 4f. a drop and a re-create of one name leave one registry entry for it, not a tombstone beside it
j 17751 create '{"col":"dup","buckets_log2":6}' > /dev/null
jseq 17751 drop '{"col":"dup"}' create '{"col":"dup","buckets_log2":6}' > /dev/null
N=$(grep -c '^collection dup ' "$D/st1/collections")
[ "$N" = 1 ] && ok "the collections file names a dropped and re-created collection once" \
	|| bad "the collections file names 'dup' $N times: $(grep '^collection dup ' "$D/st1/collections" | tr '\n' ';')"

# ---- 4g. a drop during a resize takes both tables with it; the slot's next collection starts clean
conf 3 17753 yes
start 3
j 17753 create '{"col":"rz","buckets_log2":8}' > /dev/null
python3 - <<'PYEOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 17753), timeout=30); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":{"col":"rz","key":"rz%d"%i,"value":"v"}}) for i in range(6000)]
f.write(("\n".join(reqs)+"\n").encode()); f.flush()
for _ in reqs: f.readline()
PYEOF
j 17753 resize '{"col":"rz","buckets_log2":14}' > /dev/null
# the drop lands after the swap: the live table is the new one, the old one still holds every record
i=0
while [ $i -lt 100 ]; do
	R=$(j 17753 stats '{"col":"rz"}')
	case "$R" in *'"buckets": 16384'*resizing_to*|*resizing_to*'"buckets": 16384'*) break;; esac
	sleep 0.1; i=$((i+1))
done
[ $i -lt 100 ] || bad "the resize's drain phase was never seen: $R"
j 17753 drop '{"col":"rz","force":true}' > /dev/null
sleep 3                                # the dropped table is retired and the slot reusable
j 17753 create '{"col":"rz2","buckets_log2":8}' > /dev/null
R=$(j 17753 stats '{"col":"rz2"}')
case "$R" in *resizing_to*) bad "a collection created after a drop mid-resize inherits the resize: $R";; *) ok "a collection created after a drop mid-resize is not resizing";; esac
sleep 4                                # resize ticks that would carry a stale table's records in
[ "$(entries 17753 rz2)" = 0 ] && ok "and it holds none of the dropped collection's records" \
	|| bad "rz2 holds $(entries 17753 rz2) record(s) of the dropped rz"
[ "$(bkt 17753 rz2)" = 256 ] && ok "at the size it was created at" || bad "rz2 buckets: $(bkt 17753 rz2), want 256"
stop 3

# ---- 4h. a collection change whose collections file cannot be written says so (no WAL here: the
# file is the only record, and a restart would bring a dropped collection back)
mkdir "$D/st1/collections.tmp"         # fopen of the temp file fails, as on a full or read-only dir
R=$(j 17751 create '{"col":"nofile","buckets_log2":6}')
case "$R" in *'"created": true'*'"persisted": false'*) ok "a create whose file write fails says persisted false";; *) bad "create with an unwritable file: $R";; esac
R=$(j 17751 rename '{"col":"nofile","to":"nofile2"}')
case "$R" in *'"renamed": true'*'"persisted": false'*) ok "so does a rename";; *) bad "rename with an unwritable file: $R";; esac
R=$(j 17751 drop '{"col":"nofile2"}')
case "$R" in *'"dropped": true'*'"persisted": false'*) ok "and a drop";; *) bad "drop with an unwritable file: $R";; esac
N=$(grep -c "collections file could not be written" "$D/n1.log")
[ "$N" -ge 3 ] && ok "each is logged as an error ($N)" || bad "the failed writes were logged $N times, want 3"
rmdir "$D/st1/collections.tmp"
R=$(j 17751 create '{"col":"nofile3","buckets_log2":6}')
case "$R" in *'"persisted"'*) bad "a create whose file write succeeds still carries persisted: $R";; *'"created": true'*) ok "with the file writable again the reply is as before";; *) bad "create after: $R";; esac

# ---- 5. the RESP door's index map
for i in 0 1 2 3; do j 17751 set "{\"col\":\"sbcha\",\"key\":\"s$i\",\"value\":\"v\"}" > /dev/null; done
j 17751 set '{"col":"th","key":"t0","value":"v"}' > /dev/null
j 17751 set '{"col":"hidden","key":"h","value":"v"}' > /dev/null
[ "$(resp 17851 DBSIZE)" = ":4" ] && ok "db0 is the collection the map names (sbcha, 4 keys) - not a collection literally named 0" || bad "DBSIZE db0: $(resp 17851 DBSIZE)"
[ "$(resp 17851 GET s1)" = "v" ] && ok "a RESP GET reaches it" || bad "GET: $(resp 17851 GET s1)"
echo "  ..   node 1 alive before SELECT: $(alive 1)"
[ "$(resp 17851 SELECT 1)" = "+OK" ] && ok "SELECT 1 maps to th" || bad "SELECT 1: $(resp 17851 SELECT 1)"
[ "$(resp 17851 SELECT plain)" = "+OK" ] && ok "a bare name still selects, as before" || bad "SELECT plain: $(resp 17851 SELECT plain)"
case "$(resp 17851 SELECT hidden)" in -ERR*) ok "a collection outside the allow-list is unreachable";; *) bad "SELECT hidden: $(resp 17851 SELECT hidden)";; esac
K=$(resp 17851 INFO keyspace)
case "$K" in *"db0:keys=4"*"db1:keys=1"*) ok "INFO keyspace reports every reachable collection with its real count: $K";; *) bad "INFO keyspace: $K";; esac

# ---- 6. the fleet: a create reaches the other node, and one that was down picks it up
stop 1
conf 1 17761 yes 127.0.1.81
conf 2 17762 yes 127.0.1.82
sed -i 's/advertise = 127.0.1.81/advertise = 127.0.1.81\nmode = store/' "$D/n1.conf"
sed -i 's/advertise = 127.0.1.82/advertise = 127.0.1.82\nmode = store/' "$D/n2.conf"
start 1; start 2
i=0; while [ $i -lt 100 ] && ! grep -q "joined as node" "$D/n2.log"; do sleep 0.1; i=$((i+1)); done
j 17761 create '{"col":"fleetwide","buckets_log2":8}' > /dev/null
i=0; while [ $i -lt 60 ]; do case "$(cols 17762)" in *fleetwide*) break;; esac; sleep 0.1; i=$((i+1)); done
case "$(cols 17762)" in *fleetwide*) ok "a create on node 1 reached node 2 (after $((i / 10))s)";; *) bad "node 2 has: $(cols 17762)";; esac
# a resize must reach the fleet as a resize; in store mode this is coverage only - the regression
# guard for a resize announced as a drop is test/colresizetest.sh (an eager pair)
j 17761 resize '{"col":"fleetwide","buckets_log2":10}' > /dev/null
i=0; while [ $i -lt 60 ]; do [ "$(bkt 17762 fleetwide)" = 1024 ] && break; sleep 0.1; i=$((i+1)); done
case "$(cols 17762)" in
  *fleetwide*) ok "a resize does not DELETE the collection on the peer" ;;
  *) bad "the resize removed 'fleetwide' from node 2 - announced as a drop: $(cols 17762)" ;;
esac
[ "$(bkt 17762 fleetwide)" = 1024 ] \
	&& ok "and the peer is at the new size (1024 buckets, after $((i / 10))s)" \
	|| bad "node 2 buckets for 'fleetwide': $(bkt 17762 fleetwide), want 1024"
# the originator resizes incrementally per maintenance tick while the peer adopts on receipt,
# so node 1 gets its own wait
i=0; while [ $i -lt 60 ]; do [ "$(bkt 17761 fleetwide)" = 1024 ] && break; sleep 0.5; i=$((i+1)); done
[ "$(bkt 17761 fleetwide)" = 1024 ] \
	&& ok "the originating node resized too (after $(echo "$i" | awk '{printf "%.1f", $1/2}')s)" \
	|| bad "node 1 buckets for 'fleetwide': $(bkt 17761 fleetwide), want 1024"

stop 2
j 17761 create '{"col":"whiledown","buckets_log2":8}' > /dev/null
sleep 0.5
start 2
i=0; while [ $i -lt 100 ]; do case "$(cols 17762)" in *whiledown*) break;; esac; sleep 0.1; i=$((i+1)); done
case "$(cols 17762)" in *whiledown*) ok "a node that was down through a create picks it up when it returns (after $((i / 10))s)";; *) bad "after rejoin node 2 has: $(cols 17762)";; esac
j 17761 drop '{"col":"whiledown"}' > /dev/null
i=0; while [ $i -lt 60 ]; do case "$(cols 17762)" in *whiledown*) sleep 0.1; i=$((i+1));; *) break;; esac; done
case "$(cols 17762)" in *whiledown*) bad "the drop did not reach node 2: $(cols 17762)";; *) ok "a drop reaches the fleet too";; esac

# a drop and a re-create in one breath: the old drop, re-announced every resync, must not empty the
# collection the create made, on the peer or back on the originator
j 17761 create '{"col":"dc","buckets_log2":8}' > /dev/null
i=0; while [ $i -lt 60 ]; do case "$(cols 17762)" in *dc*) break;; esac; sleep 0.1; i=$((i+1)); done
jseq 17761 drop '{"col":"dc","force":true}' create '{"col":"dc","buckets_log2":8}' > /dev/null
sleep 1
python3 - <<'PYEOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 17761), timeout=30); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":{"col":"dc","key":"d%d"%i,"value":"v"}}) for i in range(20)]
f.write(("\n".join(reqs)+"\n").encode()); f.flush()
for _ in reqs: f.readline()
PYEOF
sleep 2                                # what the writes send the peer has landed
S0=$(( $(entries 17761 dc) + $(entries 17762 dc) ))
sleep 12                               # two resyncs of the created set
S1=$(( $(entries 17761 dc) + $(entries 17762 dc) ))
[ "$S0" -ge 20 ] && [ "$S1" -ge "$S0" ] \
	&& ok "a re-created collection keeps its records through the fleet's resyncs ($S1)" \
	|| bad "the re-created 'dc' held $S0 record(s) across both nodes, $S1 after two resyncs - a stale drop emptied it"

echo "coltest: $pass passed, $fail failed"
[ $fail = 0 ]
